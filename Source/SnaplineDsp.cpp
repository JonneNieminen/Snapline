#include "SnaplineDsp.h"
#include "PitchDetector.h"
#include "LpcEnvelope.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace snapline
{
namespace
{
constexpr double twoPi = 6.283185307179586476925;

struct VoiceRange { float minHz, maxHz; };
constexpr std::array<VoiceRange, 4> voiceRanges {{
    { 130.0f, 1100.0f }, // Soprano
    { 80.0f, 800.0f },   // Alto / Tenor
    { 55.0f, 420.0f },   // Low Male
    { 45.0f, 1300.0f },  // Instrument
}};
constexpr float lowestHz = 45.0f;

constexpr double studioLatencySeconds = 0.100;
constexpr double liveLatencySeconds = 0.008;
constexpr int studioLookaheadFrames = 8;  // about 21 ms at a 375 Hz frame rate (the least Studio uses: Instrument)
constexpr int studioLookaheadMax = 20;    // about 53 ms
constexpr int studioPathLag = 5;          // Viterbi: a frame's pitch is settled 5 frames (13 ms) after it is measured
constexpr int studioSmoothLag = 12;       // zero-phase retune: the backward stage looks 12 frames (32 ms) ahead
constexpr int pathCapacity = 64;          // frames of Viterbi history (power of two, > the longest look-ahead)
constexpr float pathSigmaCents = 30.0f;   // Viterbi: pitch may move this much per 2.7 ms frame at no real cost
constexpr float pathWindowCents = 250.0f; // Viterbi candidates must lie this close to the online pitch
constexpr float pathLeapLog = -9.21f;     // log 1e-4: the cost of a jump far outside that (a leap, or a glitch and back)
constexpr float pathDropoutLog = -3.91f;  // log 0.02 per frame: "this frame's pitch is wrong, the voice carries on"
constexpr double framesPerSecond = 375.0;
constexpr float yinThreshold = 0.15f;
constexpr float voicedOnAperiodicity = 0.25f;
constexpr float voicedHoldAperiodicity = 0.35f;
constexpr float gateRms = 0.001f;         // -60 dBFS
constexpr float relativeGate = 0.08f;     // -22 dB below the recent vocal level
constexpr double holdSeconds = 0.03;      // bridge short detection dropouts inside a note
constexpr float maxDeviation = 60.0f;     // cents the pitch may stray from centre + vibrato before it drags the centre along
constexpr float springTimeScale = 2.146f; // w t at which a critically damped step response reaches 63 %
constexpr int markCapacity = 8192;
constexpr int frameCapacity = 4096;
constexpr int displayCapacity = 4096;

constexpr double lpcMaxSeconds = 0.032;     // longest LPC analysis window (low voices)
constexpr double lpcMinSeconds = 0.010;     // shortest (high voices); two periods of the lowest pitch in between
constexpr int lpcGridShift = 3;             // lattice coefficients are interpolated every 8 samples
constexpr int lpcFrameStride = 2;           // one LPC frame every 2 analysis hops (5.3 ms): formants move over 30-100 ms

constexpr int hannTableSize = 1024;
constexpr int silenceBlockShift = 5;       // 32-sample blocks flagged as digitally silent
constexpr float silenceLevel = 1.0e-7f;

/** 0.5 + 0.5 cos (pi x) sampled at x = k / hannTableSize (plus a guard entry). Symmetric, so
    table (x) + table (1 - x) = 1 and overlapping grains still sum to one. */
const std::array<float, hannTableSize + 2>& hannTable()
{
    static const auto table = [] {
        std::array<float, hannTableSize + 2> t {};
        for (int k = 0; k < hannTableSize + 2; ++k)
            t[(size_t) k] = (float) (0.5 + 0.5 * std::cos (3.141592653589793238463 * k / hannTableSize));
        return t;
    }();
    return table;
}

int nextPow2 (int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

float hermite (float xm1, float x0, float x1, float x2, float t)
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

float smoothstep (float edge0, float edge1, float x)
{
    const float t = juce::jlimit (0.0f, 1.0f, (x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

/**
    The singer's pitch centre and vibrato, tracked together by an extended Kalman filter.

    Measured pitch (cents) is modelled as a slow centre plus one sinusoid plus measurement noise:
        z_k = c_k + p_k + v_k,
    with the state x = [c, p, q, w]:
        c_{k+1} = c_k                                  + random walk (the centre drifts slowly)
        [p; q]_{k+1} = R(w_k dt) [p; q]_k              + noise (vibrato depth and phase drift)
        w_{k+1} = w_k                                  + random walk (the rate drifts)
    where R is a rotation, so (p, q) is a quadrature oscillator at w rad/s: p is the vibrato
    excursion and sqrt(p^2 + q^2) its depth. The rate w is part of the state, which makes the
    model non-linear; the EKF linearises the rotation about the current estimate:
        dp'/dw = dt (-sin(w dt) p - cos(w dt) q),   dq'/dw = dt (cos(w dt) p - sin(w dt) q).
    This is the classic EKF frequency tracker (La Scala & Bitmead, "Design of an extended Kalman
    filter frequency tracker", IEEE Trans. Signal Processing 44(3), 1996) with a random-walk level
    added. The rate is kept within 4-8 Hz, where sung vibrato lives (Prame, "Measurements of the
    vibrato rate of ten singers", JASA 96, 1994: 5.5-6.7 Hz on long notes, rising at note ends).

    Why: a low-pass centre (the old two cascaded 3 Hz poles) leaks part of a 5-6 Hz vibrato with a
    phase lag, so "correct the centre, keep the deviation" still nudged the vibrato. Here the
    oscillator absorbs the vibrato and the centre only sees what is left. With no vibrato the
    oscillator's depth stays near zero and the centre follows the pitch like a one-pole low-pass
    of about 2 Hz (for a random walk observed in white noise the steady-state Kalman gain is
    alpha ~ sqrt(Qc / R) per step).

    Doubles, four states, about 150 multiply-adds per analysis frame (375 frames a second).
*/
struct VibratoTracker
{
    double x[4] {};      // centre (relative to `origin`), p, q (cents), w (rad/s)
    double P[4][4] {};
    double origin = 0.0; // the centre is tracked relative to this, so the numbers stay small

    static constexpr double minRate = 2.0 * 3.141592653589793 * 4.0;
    static constexpr double maxRate = 2.0 * 3.141592653589793 * 8.0;
    static constexpr double startRate = 2.0 * 3.141592653589793 * 5.5;
    static constexpr double measurementVar = 64.0; // (8 ct)^2: YIN noise and micro-movement
    static constexpr double centreVar = 0.07;      // per frame: about a 2 Hz centre at dt = 2.7 ms
    static constexpr double vibratoVar = 2.5;      // per frame: depth may change by ~10 ct in 0.1 s
    static constexpr double rateVar = 0.35;        // per frame: about 1 Hz of rate drift in 0.3 s
    static constexpr double rateVarMax = (2.0 * 3.141592653589793 * 1.5) * (2.0 * 3.141592653589793 * 1.5);

    void reset (float cents)
    {
        origin = cents;
        x[0] = x[1] = x[2] = 0.0;
        x[3] = startRate;
        for (auto& row : P)
            for (auto& v : row) v = 0.0;
        P[0][0] = measurementVar;
        P[1][1] = P[2][2] = 400.0; // the vibrato may already be up to ~40 ct
        P[3][3] = rateVarMax;
    }

    float centre() const { return (float) (origin + x[0]); }
    float vibrato() const { return (float) x[1]; }
    float depth() const { return (float) std::sqrt (x[1] * x[1] + x[2] * x[2]); }

    void update (float measured, double dt)
    {
        // Predict.
        const double th = x[3] * dt, cs = std::cos (th), sn = std::sin (th);
        const double p = x[1], q = x[2];
        double F[4][4] {};
        F[0][0] = 1.0;
        F[1][1] = cs; F[1][2] = -sn; F[1][3] = dt * (-sn * p - cs * q);
        F[2][1] = sn; F[2][2] = cs;  F[2][3] = dt * (cs * p - sn * q);
        F[3][3] = 1.0;
        x[1] = cs * p - sn * q;
        x[2] = sn * p + cs * q;
        double FP[4][4] {};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    FP[i][j] += F[i][k] * P[k][j];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                double s = 0.0;
                for (int k = 0; k < 4; ++k)
                    s += FP[i][k] * F[j][k];
                P[i][j] = s;
            }
        P[0][0] += centreVar;
        P[1][1] += vibratoVar;
        P[2][2] += vibratoVar;
        P[3][3] = std::min (P[3][3] + rateVar, rateVarMax); // the rate is unobservable without vibrato: cap its uncertainty

        // Update with z = c + p (H = [1 1 0 0]).
        const double innovation = (double) measured - origin - x[0] - x[1];
        const double s = P[0][0] + P[1][1] + 2.0 * P[0][1] + measurementVar;
        double K[4], HP[4];
        for (int i = 0; i < 4; ++i)
        {
            K[i] = (P[i][0] + P[i][1]) / s;
            HP[i] = P[0][i] + P[1][i];
        }
        for (int i = 0; i < 4; ++i)
            x[i] += K[i] * innovation;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                P[i][j] -= K[i] * HP[j];
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                P[i][j] = P[j][i] = 0.5 * (P[i][j] + P[j][i]);
        x[3] = juce::jlimit (minRate, maxRate, x[3]);
        // A step or a glide can be soaked up by the oscillator (centre and vibrato are told apart
        // only by their dynamics). Sung vibrato rarely exceeds +-60 ct (Prame 1994), so the depth
        // is limited to twice that; the rest of a step goes to the centre.
        const double d = std::sqrt (x[1] * x[1] + x[2] * x[2]);
        if (d > maxDepth)
        {
            const double excess = x[1] * (1.0 - maxDepth / d);
            x[0] += excess;
            x[1] *= maxDepth / d;
            x[2] *= maxDepth / d;
        }
    }
    static constexpr double maxDepth = 120.0;
};
}

struct SnaplineDsp::Impl
{
    struct Mark
    {
        std::int64_t pos = 0;
        bool voiced = false;
    };

    struct Frame
    {
        std::int64_t time = 0;     // centre of the analysis window (input timeline)
        float hz = 0.0f;           // detected (0 = unvoiced)
        float shiftCents = 0.0f;   // total shift applied to grains around this frame
        bool voiced = false;       // voiced or bridged by the hold
        // Studio's zero-phase retune (see finishCorrection): what the causal half left for the backward half.
        std::int8_t kind = 0;      // 0 unvoiced, 1 bridged, 2 measured
        float pull = 0.0f;         // output of the spring's first (forward) stage
        float alpha = 0.0f;        // that stage's coefficient at this frame
        float fullError = 0.0f;    // the correction that would put the singer exactly on the note
        float extra = 0.0f;        // natural vibrato, synthetic vibrato and transpose (cents)
        float amount = 1.0f;
        float causal = 0.0f;       // the ordinary (forward) spring's output
        float zeroPhaseWeight = 1.0f; // 0 in an attack (keeps the spring's lag), 1 once the note has settled
        int target = -1;
    };

    double fs = 48000.0;
    int hop = 128;
    int maxWindow = 0;
    int unvoicedPeriod = 256;
    int ringSize = 0, ringMask = 0;

    std::vector<float> input, lowpassed, dryL, dryR, accL, accR, win, scratch;
    bool stereo = false;                  // two channels in: both are resynthesised from the shared pitch marks
    std::vector<std::uint8_t> blockLoud;   // per 32-sample block of `input`: any sample above -140 dB
    float blockPeak = 0.0f;
    int hopCounter = 0;
    PitchDetector detector;

    std::int64_t now = 0;                 // samples received
    std::vector<Mark> marks;
    std::int64_t markCount = 0;
    std::vector<Frame> frames;
    std::int64_t frameCount = 0;          // frames analysed

    // Studio's fixed-lag Viterbi over the detector's candidates (see viterbiStep).
    struct PathFrame
    {
        int count = 0;                                            // 0 = unvoiced (one free state)
        std::array<float, PitchDetector::maxCandidates + 2> cents {}; // 1200 log2 (hz)
        std::array<float, PitchDetector::maxCandidates + 2> score {};
        std::array<std::int8_t, PitchDetector::maxCandidates + 2> back {};
        std::array<bool, PitchDetector::maxCandidates + 2> dropout {};
    };
    std::vector<PathFrame> path;
    int pathLag = 0;                      // frames between a measurement and its final pitch
    std::int64_t correctedCount = 0;      // frames with a correction decided
    std::int64_t preparedCount = 0;       // frames through the causal half of the decision (Studio: ahead of correctedCount)
    int smoothLag = 0;                    // Studio: frames the backward half of the retune spring looks ahead

    Mode mode = Mode::studio;
    int latency = 0;
    int lookahead = 0;
    double synthPos = 0.0;                // next grain centre (output timeline)
    double extraDelay = 0.0;              // live-mode adaptive delay on top of `latency`
    double delayFloor = 0.0;              // what this voice has needed lately; pauses never relax below it (decays over ~4 s)
    float lp1 = 0.0f, lp2 = 0.0f, lpCoeff = 0.0f;

    // Tracker state
    bool trackerVoiced = false;
    float lastVoicedHz = 0.0f;
    float suspectHz = 0.0f;               // a large jump seen once, waiting for confirmation
    float lastShift = 0.0f;
    int holdFramesLeft = 0;
    float recentHz[3] {};
    float levelEnvelope = 0.0f;

    // Correction state
    int currentNote = -1;
    double noteAge = 0.0;
    float correction = 0.0f;              // output of the retune spring (cents)
    float correctionPull = 0.0f;          // its first stage: the spring's anchor position
    float correctionForward = 0.0f;       // Studio: the second stage run forwards too (attacks keep its lag)
    double vibratoPhase = 0.0;
    VibratoTracker vibratoTracker;        // the singer's pitch centre and vibrato (cents)
    bool centreValid = false;

    // Output smoothing
    float mixSmoothed = 1.0f, gainSmoothed = 1.0f, bypassSmoothed = 0.0f; // bypassSmoothed: 1 = fully dry

    juce::AbstractFifo displayFifo { displayCapacity };
    std::array<DisplayFrame, displayCapacity> displayFrames {};

    // LPC formant methods (all buffers allocated in prepare, used only when the method is not classic)
    struct LpcFrame
    {
        std::int64_t time = 0;                   // input time this envelope belongs to (interpolation knot)
        float error = 0.0f;                      // prediction error power E (0 = silent frame)
        std::array<float, maxLpcOrder> k {};     // reflection coefficients of A(z)
        std::array<float, maxLpcOrder> kWarp {}; // the same envelope moved by warpPhi (computed lazily)
        float warpGain = 1.0f;
        double warpPhi = 0.0;                    // 0 = kWarp not computed yet
    };
    FormantMethod method = FormantMethod::classic;
    LpcAnalyzer lpc;
    int lpcOrder = 16, lpcMaxWindow = 0;
    std::vector<LpcFrame> lpcFrames;
    std::int64_t lpcCount = 0;
    std::vector<float> resL, resR;               // source-filter: LPC residual (excitation) per channel
    std::vector<float> delayAcc;                 // window-weighted source delay of the grains at each output sample
    std::int64_t residualPos = 0;                // residual is known for input times below this
    LatticeInverse inputInvL, inputInvR, outInvL, outInvR;
    LatticeSynthesis outSynL, outSynR;
    std::array<float, maxLpcOrder> kIn {}, kSrcOut {}, kWarpOut {};
    float gainOut = 1.0f;
    bool envelopeFilterOn = false, lastWetHeard = true;
    bool lpcAnalysing = false;                   // source-filter always; envelope only while something moves
    float lpcLevel = 0.0f;                       // recent peak of the LPC frame energy (decays over ~1 s)
    float guardWet = 0.0f, guardDry = 0.0f;      // re-colouring guard: short-term power of the output and of its source

    const SnaplineParams* params = nullptr;
    std::atomic<float> effectiveDelay { 0.0f };

    // ------------------------------------------------------------------
    void prepare (double sampleRate, Mode initialMode)
    {
        fs = sampleRate;
        hop = juce::jmax (32, (int) std::lround (fs / framesPerSecond));
        maxWindow = (int) std::ceil (fs / lowestHz) + 2;
        unvoicedPeriod = juce::jmax (16, (int) std::lround (fs / 180.0));
        ringSize = nextPow2 ((int) (fs * (studioLatencySeconds + 0.3)) + 8 * maxWindow);
        ringMask = ringSize - 1;
        for (auto* v : { &input, &lowpassed, &dryL, &dryR, &accL, &accR, &win })
            v->assign ((size_t) ringSize, 0.0f);
        scratch.assign ((size_t) (2 * maxWindow + 4), 0.0f);
        blockLoud.assign ((size_t) (ringSize >> silenceBlockShift), (std::uint8_t) 0);
        detector.prepare (fs, maxWindow);
        marks.assign ((size_t) markCapacity, {});
        frames.assign ((size_t) frameCapacity, {});
        path.assign ((size_t) pathCapacity, {});
        lpCoeff = (float) (1.0 - std::exp (-twoPi * 900.0 / fs));
        lpcMaxWindow = (int) std::ceil (lpcMaxSeconds * fs) + 2;
        lpc.prepare (fs, lpcMaxWindow);
        lpcOrder = lpc.order();
        lpcFrames.assign ((size_t) frameCapacity, {});
        resL.assign ((size_t) ringSize, 0.0f);
        resR.assign ((size_t) ringSize, 0.0f);
        delayAcc.assign ((size_t) ringSize, 0.0f);
        if ((int) scratch.size() < lpcMaxWindow)
            scratch.assign ((size_t) lpcMaxWindow, 0.0f);
        mode = initialMode;
        reset();
    }

    void reset()
    {
        for (auto* v : { &input, &lowpassed, &dryL, &dryR, &accL, &accR, &win })
            std::fill (v->begin(), v->end(), 0.0f);
        now = 0;
        hopCounter = 0;
        blockPeak = 0.0f;
        std::fill (blockLoud.begin(), blockLoud.end(), (std::uint8_t) 0);
        markCount = 0;
        pushMark (0, false);
        frameCount = 0;
        correctedCount = 0;
        preparedCount = 0;
        for (auto& pf : path) pf.count = 0;
        lp1 = lp2 = 0.0f;
        trackerVoiced = false;
        lastVoicedHz = 0.0f;
        suspectHz = 0.0f;
        lastShift = 0.0f;
        holdFramesLeft = 0;
        levelEnvelope = 0.0f;
        std::fill (std::begin (recentHz), std::end (recentHz), 0.0f);
        currentNote = -1;
        noteAge = 0.0;
        correction = 0.0f;
        correctionPull = 0.0f;
        correctionForward = 0.0f;
        vibratoPhase = 0.0;
        vibratoTracker.reset (0.0f);
        centreValid = false;
        bypassSmoothed = 0.0f;
        applyMode (mode);
        synthPos = (double) latency;
        extraDelay = 0.0;
        delayFloor = 0.0;
        std::fill (resL.begin(), resL.end(), 0.0f);
        std::fill (resR.begin(), resR.end(), 0.0f);
        std::fill (delayAcc.begin(), delayAcc.end(), 0.0f);
        lpcCount = 0;
        lpcLevel = 0.0f;
        guardWet = guardDry = 0.0f;
        residualPos = now;
        resetLattices();
        displayFifo.reset();
    }

    void resetLattices()
    {
        for (auto* l : { &inputInvL, &inputInvR, &outInvL, &outInvR })
            l->reset();
        outSynL.reset();
        outSynR.reset();
    }

    // ------------------------------------------------------------------ LPC envelope
    LpcFrame& lpcFrame (std::int64_t j) { return lpcFrames[(size_t) (j & (frameCapacity - 1))]; }
    std::int64_t oldestLpc() const { return juce::jmax<std::int64_t> (0, lpcCount - frameCapacity + 4); }

    int lpcWindowLength() const
    {
        const int twoPeriods = (int) std::ceil (2.0 * fs / range().minHz);
        return juce::jlimit ((int) (lpcMinSeconds * fs), lpcMaxWindow - 2, twoPeriods);
    }

    /**
        One LPC frame from the newest input. Its knot time is where it applies: in Studio the
        window's centre (the look-ahead pays for it), in Live one hop ahead of now, so the input
        can be inverse-filtered as soon as it arrives (the envelope estimate then lags by half a
        window, about 8-16 ms, which formants, moving over 30-100 ms, barely notice).
    */
    void analyseLpc()
    {
        const int n = lpcWindowLength();
        for (int i = 0; i < n; ++i)
            scratch[(size_t) i] = sampleAt (input, now - n + i);
        auto& f = lpcFrame (lpcCount);
        const std::int64_t knot = mode == Mode::studio ? now - n / 2 : now + (std::int64_t) (lpcFrameStride * hop);
        f.time = lpcCount > 0 ? juce::jmax (knot, lpcFrame (lpcCount - 1).time + 1) : knot;
        float energy = 0.0f;
        for (int i = 0; i < n; ++i)
            energy += scratch[(size_t) i] * scratch[(size_t) i];
        energy /= (float) n;
        lpcLevel = juce::jmax (energy, lpcLevel * std::exp (-(float) (lpcFrameStride * hop) / (float) fs));
        // Quiet frames (pauses, more than 40 dB under the recent voice) keep the last real envelope
        // instead of fitting the noise floor. The vocal tract does not vanish between notes, and an
        // onset is then whitened and re-coloured with similar resonances on both sides of the
        // grains that straddle it (a near-flat envelope against a sharp one made bursts there).
        if (energy < 1.0e-4f * lpcLevel && lpcCount > 0)
        {
            const auto& previous = lpcFrame (lpcCount - 1);
            f.k = previous.k;
            f.error = previous.error;
        }
        else
            lpc.analyse (scratch.data(), n, f.k.data(), f.error); // (DAP and True Envelope were compared for high voices: see ARCHITECTURE.md section 8)
        f.warpPhi = 0.0;
        ++lpcCount;
    }

    /**
        Start the LPC history again (method or mode change). The first frame is analysed at once
        and the residual is rebuilt for the audio the grains may still read, all with that frame;
        times before the first knot use it on both sides (whitening and re-colouring), so the
        pair stays an exact inverse.
    */
    void restartLpc()
    {
        lpcCount = 0;
        resetLattices();
        envelopeFilterOn = false;
        lpcAnalysing = method == FormantMethod::lpcSourceFilter || (method == FormantMethod::lpcEnvelope && envelopeMoves (*params));
        if (! lpcAnalysing)
            return;
        analyseLpc();
        const auto backfill = (std::int64_t) latency + 4 * (std::int64_t) maxWindow + (std::int64_t) (0.05 * fs);
        residualPos = juce::jmax<std::int64_t> (0, now - backfill);
    }

    static bool envelopeMoves (const SnaplineParams& p)
    {
        return p.formantShift != 0.0f || p.throat != 100.0f || ! p.preserveFormants;
    }

    /** Frames around time t and the interpolation weight between them; false when there are none. */
    bool lpcBracket (std::int64_t t, std::int64_t& j0, std::int64_t& j1, float& w)
    {
        if (lpcCount == 0)
            return false;
        std::int64_t lo = oldestLpc(), hi = lpcCount - 1;
        w = 0.0f;
        if (t <= lpcFrame (lo).time) { j0 = j1 = lo; return true; }
        if (t >= lpcFrame (hi).time) { j0 = j1 = hi; return true; }
        while (hi - lo > 1)
        {
            const auto mid = (lo + hi) / 2;
            (lpcFrame (mid).time <= t ? lo : hi) = mid;
        }
        j0 = lo;
        j1 = hi;
        w = (float) (t - lpcFrame (lo).time) / (float) juce::jmax<std::int64_t> (1, lpcFrame (hi).time - lpcFrame (lo).time);
        return true;
    }

    /** Source envelope at t (reflection coefficients, linearly interpolated between knots). */
    void sourceCoefficients (std::int64_t t, float* k)
    {
        std::int64_t j0, j1;
        float w;
        if (! lpcBracket (t, j0, j1, w))
        {
            std::fill (k, k + lpcOrder, 0.0f);
            return;
        }
        const auto& a = lpcFrame (j0).k;
        const auto& b = lpcFrame (j1).k;
        for (int m = 0; m < lpcOrder; ++m)
            k[m] = a[(size_t) m] + w * (b[(size_t) m] - a[(size_t) m]);
    }

    /** How far the envelope moves for the frame at time t: Formant and Throat, plus the pitch ratio when formants are not preserved. */
    double envelopeFactor (std::int64_t t)
    {
        const auto& p = *params;
        double phi = std::exp2 ((double) p.formantShift / 12.0) * (100.0 / juce::jlimit (50.0, 200.0, (double) p.throat));
        if (! p.preserveFormants)
            phi *= std::exp2 ((double) juce::jlimit (-2400.0f, 2400.0f, shiftAt (t)) / 1200.0);
        return juce::jlimit (0.5, 2.0, phi);
    }

    void ensureWarp (LpcFrame& f)
    {
        const double phi = envelopeFactor (f.time);
        if (std::abs (f.warpPhi - phi) > 1.0e-4)
        {
            lpc.warp (f.k.data(), f.error, phi, f.kWarp.data(), f.warpGain);
            f.warpPhi = phi;
        }
    }

    /** Target (warped) envelope at t and its level correction. */
    void targetCoefficients (std::int64_t t, float* k, float& gain)
    {
        std::int64_t j0, j1;
        float w;
        if (! lpcBracket (t, j0, j1, w))
        {
            std::fill (k, k + lpcOrder, 0.0f);
            gain = 1.0f;
            return;
        }
        auto& a = lpcFrame (j0);
        auto& b = lpcFrame (j1);
        ensureWarp (a);
        if (j1 != j0)
            ensureWarp (b);
        for (int m = 0; m < lpcOrder; ++m)
            k[m] = a.kWarp[(size_t) m] + w * (b.kWarp[(size_t) m] - a.kWarp[(size_t) m]);
        gain = a.warpGain + w * (b.warpGain - a.warpGain);
    }

    /**
        Source-filter: inverse-filter the new input, e[t] = A_t(z) x[t], for every time whose
        envelope is final (below the newest knot). The coefficients are a pure function of the
        8-sample grid time, so the re-colouring filter later sees exactly the same A_t.
    */
    void advanceResidual()
    {
        if (lpcCount == 0)
            return;
        const auto limit = juce::jmin (now, lpcFrame (lpcCount - 1).time);
        constexpr std::int64_t gridMask = (1 << lpcGridShift) - 1;
        for (auto t = residualPos; t < limit; ++t)
        {
            if ((t & gridMask) == 0 || t == residualPos)
                sourceCoefficients (t & ~gridMask, kIn.data());
            const auto idx = (size_t) (t & ringMask);
            resL[idx] = inputInvL.process (dryL[idx], kIn.data(), lpcOrder);
            if (stereo)
                resR[idx] = inputInvR.process (dryR[idx], kIn.data(), lpcOrder);
        }
        residualPos = juce::jmax (residualPos, limit);
    }


    int latencyFor (Mode m) const
    {
        return (int) std::lround (fs * (m == Mode::studio ? studioLatencySeconds : liveLatencySeconds));
    }

    void applyMode (Mode m)
    {
        // Frames already through the causal half are finished with what is known (look-ahead shrinks).
        if (params != nullptr)
            while (correctedCount < preparedCount)
                finishCorrection (correctedCount++);
        mode = m;
        latency = latencyFor (m);
        lookahead = m == Mode::studio ? studioLookahead() : 0;
        // The causal half must see 3 frames past k (the median), each final after pathLag frames.
        smoothLag = m == Mode::studio ? juce::jmax (0, juce::jmin (studioSmoothLag, lookahead - pathLag - 3)) : 0;
        pathLag = m == Mode::studio ? juce::jmin (studioPathLag, lookahead - 3) : 0; // the median reads 3 frames ahead
        for (auto& pf : path) pf.count = 0;
    }

    /**
        How many frames Studio may decide late. A grain sourced at u = s - latency needs the frames
        around u decided; at output time s the newest frame lies about 1.5 W (YIN's window, see
        analyseFrame) before s, and a grain's centre can lie up to one period (its left half) after
        the block it starts in. So lookahead * hop <= latency - 1.5 W - longest period, less two hops
        of margin: about 20 frames (53 ms) for Soprano and Alto/Tenor, 18 for Low Male and 14 for
        Instrument at Studio's 100 ms. The block size does not matter (a block is fully analysed
        before its grains are placed).
    */
    int studioLookahead() const
    {
        if (params == nullptr)
            return studioLookaheadFrames;
        const auto& r = range();
        const double window = std::ceil (fs / r.minHz) + 2.0, period = fs / (0.9 * r.minHz);
        const int fit = (int) std::floor ((latencyFor (Mode::studio) - 1.5 * window - period - 2.0 * hop) / hop);
        return juce::jlimit (studioLookaheadFrames, studioLookaheadMax, fit);
    }

    /**
        One step of a fixed-lag Viterbi decoder over the detector's pitch candidates (Studio).

        pYIN (Mauch & Dixon, "pYIN: a fundamental frequency estimator using probabilistic threshold
        distributions", ICASSP 2014) scores several YIN dips per frame and then finds the most
        likely pitch path through them with a hidden Markov model. The engine's online tracker
        collapses that into one step (a continuity prior on the previous pitch). Studio's look-ahead
        allows the real thing over a short window: the most likely path is decoded at the newest
        frame and read back `pathLag` frames, where later evidence has had its say (a fixed-lag
        smoother; the full pYIN decodes the whole file).

        States of a frame: each candidate within 250 ct of the online pitch (log observation = log of
        its pYIN probability), the
        online tracker's own pitch (after its octave fold), and a dropout state ("this frame's
        measurement is wrong, the voice carries on at the previous pitch", log 0.02 per frame).
        Observations are normalised within the frame (the best candidate scores log 1).
        Transitions: log p = -0.5 (delta cents / 30)^2, floored at log 1e-4, so smooth movement is
        free and a jump costs the same whether it is a leap or a glitch; a glitch must also jump
        back, so a one- to four-frame spike costs more than the dropout path and is removed, while a
        real leap is cheaper than staying in dropout after two or three frames and is kept. Voicing
        stays the online tracker's decision (its level gates are well tuned); unvoiced frames have
        one state that any candidate may leave from freely.

        Pitch candidates are the only places the path can be (as in pYIN, bins without a candidate
        have zero observation probability), so the work is at most 26 x 26 transitions a frame.
    */
    void viterbiStep (const PitchDetector::Result& res, const Frame& f)
    {
        auto& cur = path[(size_t) (frameCount & (pathCapacity - 1))];
        const auto& prev = path[(size_t) ((frameCount - 1) & (pathCapacity - 1))];
        const bool havePrev = frameCount > 0 && prev.count > 0;
        if (! f.voiced)
        {
            cur.count = 0;
            float bestScore = 0.0f;
            int bestState = 0;
            for (int j = 0; j < (havePrev ? prev.count : 0); ++j)
                if (j == 0 || prev.score[(size_t) j] > bestScore) { bestScore = prev.score[(size_t) j]; bestState = j; }
            cur.back[0] = (std::int8_t) bestState;
            return;
        }
        if (f.hz <= 0.0f) // bridged by the hold: no measurement, every hypothesis carries on
        {
            if (havePrev)
            {
                cur = prev;
                for (int j = 0; j < cur.count; ++j)
                    cur.back[(size_t) j] = (std::int8_t) j;
            }
            else
                cur.count = 0;
            return;
        }
        const auto& r = range();
        int n = 0;
        float bestProbability = 1.0e-3f;
        for (int i = 0; i < res.candidateCount; ++i)
        {
            const auto& c = res.candidates[i];
            // Octave and sub-harmonic decisions stay with the online tracker (its fold and two-frame
            // confirmation); the path chooses among candidates near its pitch, which is where the
            // one-off glitches are. (Free choice let a note start on its sub-octave.)
            if (c.hz < r.minHz * 0.9f || c.hz > r.maxHz * 1.1f || std::abs (1200.0f * std::log2 (c.hz / f.hz)) > pathWindowCents)
                continue;
            cur.cents[(size_t) n] = 1200.0f * std::log2 (c.hz);
            cur.score[(size_t) n] = std::log (c.probability + 0.01f);
            cur.dropout[(size_t) n] = false;
            bestProbability = juce::jmax (bestProbability, c.probability);
            ++n;
        }
        cur.cents[(size_t) n] = 1200.0f * std::log2 (f.hz); // the online choice, as likely as the best dip
        cur.score[(size_t) n] = std::log (bestProbability + 0.01f);
        cur.dropout[(size_t) n] = false;
        ++n;
        // Voicing is decided elsewhere, so only the candidates' relative likelihood matters: the best
        // one gets log 1. (Raw probabilities are small on breathy frames and lost to the dropout state.)
        for (int i = 0; i < n; ++i)
            cur.score[(size_t) i] -= std::log (bestProbability + 0.01f);
        for (int i = 0; i < n; ++i)
        {
            float best = 0.0f;
            int from = 0;
            if (havePrev)
            {
                best = -1.0e30f;
                for (int j = 0; j < prev.count; ++j)
                {
                    const float z = (cur.cents[(size_t) i] - prev.cents[(size_t) j]) / pathSigmaCents;
                    const float t = prev.score[(size_t) j] + juce::jmax (-0.5f * z * z, pathLeapLog);
                    if (t > best) { best = t; from = j; }
                }
            }
            cur.score[(size_t) i] += best;
            cur.back[(size_t) i] = (std::int8_t) from;
        }
        // Dropout: continue the most likely previous state at its pitch.
        if (havePrev)
        {
            int from = 0;
            for (int j = 1; j < prev.count; ++j)
                if (prev.score[(size_t) j] > prev.score[(size_t) from]) from = j;
            cur.cents[(size_t) n] = prev.cents[(size_t) from];
            cur.score[(size_t) n] = prev.score[(size_t) from] + pathDropoutLog;
            cur.back[(size_t) n] = (std::int8_t) from;
            cur.dropout[(size_t) n] = true;
            ++n;
        }
        cur.count = n;
        float top = cur.score[0];
        for (int i = 1; i < n; ++i) top = juce::jmax (top, cur.score[(size_t) i]);
        for (int i = 0; i < n; ++i) cur.score[(size_t) i] -= top; // keep the numbers small
    }

    /** Backtrack from the newest frame's best state and settle the pitch of the frame `pathLag` back. */
    void viterbiSettle()
    {
        const auto newest = frameCount - 1;
        const auto target = newest - pathLag;
        if (target < 0 || target < oldestFrame())
            return;
        const auto& last = path[(size_t) (newest & (pathCapacity - 1))];
        int state = 0;
        for (int i = 1; i < last.count; ++i)
            if (last.score[(size_t) i] > last.score[(size_t) state]) state = i;
        for (auto t = newest; t > target; --t)
            state = path[(size_t) (t & (pathCapacity - 1))].back[(size_t) juce::jmax (0, state)];
        auto& f = frame (target);
        const auto& pf = path[(size_t) (target & (pathCapacity - 1))];
        if (f.hz > 0.0f && pf.count > 0 && state < pf.count)
            f.hz = pf.dropout[(size_t) state] ? 0.0f : std::exp2 (pf.cents[(size_t) state] / 1200.0f);
    }

    // ------------------------------------------------------------------ rings
    const Mark& mark (std::int64_t j) const { return marks[(size_t) (j & (markCapacity - 1))]; }
    Frame& frame (std::int64_t k) { return frames[(size_t) (k & (frameCapacity - 1))]; }
    std::int64_t oldestMark() const { return juce::jmax<std::int64_t> (0, markCount - markCapacity + 4); }
    std::int64_t oldestFrame() const { return juce::jmax<std::int64_t> (0, frameCount - frameCapacity + 4); }

    void pushMark (std::int64_t pos, bool voiced)
    {
        marks[(size_t) (markCount & (markCapacity - 1))] = { pos, voiced };
        ++markCount;
    }

    float sampleAt (const std::vector<float>& ring, std::int64_t t) const
    {
        return t < 0 ? 0.0f : ring[(size_t) (t & ringMask)];
    }

    float interpolatedAt (const std::vector<float>& ring, double position) const
    {
        const auto base = (std::int64_t) std::floor (position);
        const float frac = (float) (position - (double) base);
        return hermite (sampleAt (ring, base - 1), sampleAt (ring, base), sampleAt (ring, base + 1), sampleAt (ring, base + 2), frac);
    }

    // ------------------------------------------------------------------ analysis
    const VoiceRange& range() const { return voiceRanges[(size_t) juce::jlimit (0, 3, (int) params->inputType)]; }

    void analyseFrame()
    {
        const auto& r = range();
        const int window = juce::jmin (maxWindow, (int) std::ceil (fs / r.minHz) + 2);
        const int minLag = juce::jmax (2, (int) std::floor (fs / r.maxHz));
        Frame f;
        f.time = now - window;

        // Frames that cannot pass the level gates below skip the FFT inside detect().
        const float decay = std::exp (-(float) hop / (float) (0.6 * fs));
        PitchDetector::Result res;
        if (now >= 2 * window)
        {
            const std::int64_t start = now - 2 * window;
            for (int i = 0; i < 2 * window; ++i)
                scratch[(size_t) i] = input[(size_t) ((start + i) & ringMask)];
            res = detector.detect (scratch.data(), window, minLag, yinThreshold,
                                   juce::jmax (gateRms, levelEnvelope * decay * relativeGate),
                                   trackerVoiced ? lastVoicedHz : 0.0f);
        }

        const bool inRange = res.hz >= r.minHz * 0.9f && res.hz <= r.maxHz * 1.1f;
        const float limit = trackerVoiced ? voicedHoldAperiodicity : voicedOnAperiodicity;
        // Level relative to the recent vocal: quiet note tails and room noise are
        // not tracked (they invite octave errors), the hold below bridges the edge.
        levelEnvelope = juce::jmax (res.rms, levelEnvelope * decay);
        const bool loudEnough = res.rms > gateRms && res.rms > levelEnvelope * relativeGate;
        bool voiced = inRange && loudEnough && res.aperiodicity < limit;

        // Jump guard for a voiced run. A detection that is two to six times off the previous
        // pitch is almost always the detector landing on a multiple (or a harmonic) of the real
        // period; fold it back. Any other jump of more than three semitones must show up in two
        // frames in a row before it is believed (a legato leap does; a one-frame glitch does not).
        if (voiced && trackerVoiced && lastVoicedHz > 0.0f)
        {
            const float ratio = res.hz / lastVoicedHz;
            for (int n = 2; n <= 6; ++n)
            {
                if (std::abs (1200.0f * std::log2 (ratio * (float) n)) < 140.0f) { res.hz *= (float) n; break; }
                if (std::abs (1200.0f * std::log2 (ratio / (float) n)) < 140.0f) { res.hz /= (float) n; break; }
            }
            if (std::abs (1200.0f * std::log2 (res.hz / lastVoicedHz)) > 300.0f)
            {
                if (suspectHz > 0.0f && std::abs (1200.0f * std::log2 (res.hz / suspectHz)) < 60.0f)
                    suspectHz = 0.0f;          // confirmed: a real leap
                else
                {
                    suspectHz = res.hz;
                    voiced = false;            // ignore this frame; the hold below keeps the note going
                }
            }
            else
                suspectHz = 0.0f;
        }
        else
            suspectHz = 0.0f;

        if (voiced)
        {
            f.hz = res.hz;
            f.voiced = true;

            lastVoicedHz = res.hz;
            holdFramesLeft = (int) std::ceil (holdSeconds * fs / hop);
        }
        else if (holdFramesLeft > 0 && loudEnough && lastVoicedHz > 0.0f)
        {
            --holdFramesLeft;
            f.hz = 0.0f;       // no new measurement
            f.voiced = true;   // but keep the note going
            voiced = true;
        }
        else
        {
            holdFramesLeft = 0;
        }
        trackerVoiced = voiced;
        // The frame's time is where YIN actually measured. d(tau) = sum_{j<W} (x_j - x_{j+tau})^2
        // over a frame that starts at now - 2W uses samples [0, W + tau), so its centre is
        // now - 2W + (W + tau) / 2, about half a window minus half a period earlier than the window
        // centre that was used before (4-5 ms for an alto). The shift is laid on grains by this time,
        // so with the old one every correction was applied that much late: a 5.6 Hz vibrato was only
        // partly flattened (21 % of it was left at -100 %, 6 % now) and hard tune spread 5 ct.
        // Unvoiced frames use the last period; times stay increasing for the searches below.
        {
            const double lag = f.hz > 0.0f ? fs / f.hz : lastVoicedHz > 0.0f ? fs / lastVoicedHz : 0.5 * window;
            f.time = now - 2 * window + (window + (std::int64_t) std::lround (lag)) / 2;
            if (frameCount > 0)
                f.time = juce::jmax (f.time, frame (frameCount - 1).time + 1);
        }

        // Median of the last three measured periods drives pitch-mark spacing.
        if (f.hz > 0.0f)
        {
            recentHz[0] = recentHz[1];
            recentHz[1] = recentHz[2];
            recentHz[2] = f.hz;
        }

        if (pathLag > 0)
            viterbiStep (res, f);
        frame (frameCount) = f;
        ++frameCount;
        if (pathLag > 0)
            viterbiSettle();

        while (preparedCount < frameCount - (lookahead - smoothLag))
            decideCorrection (preparedCount++);
        if (smoothLag == 0)
            correctedCount = preparedCount;
        while (correctedCount < preparedCount - smoothLag)
            finishCorrection (correctedCount++);

        if (lpcAnalysing && (frameCount % lpcFrameStride) == 0)
            analyseLpc();
    }

    float markPeriod() const
    {
        if (! trackerVoiced)
            return (float) unvoicedPeriod;
        float a = recentHz[0], b = recentHz[1], c = recentHz[2];
        if (a <= 0.0f) a = c;
        if (b <= 0.0f) b = c;
        const float median = std::max (std::min (a, b), std::min (std::max (a, b), c));
        if (median <= 0.0f)
            return (float) unvoicedPeriod;
        const auto& r = range();
        return (float) (fs / juce::jlimit (r.minHz * 0.9f, r.maxHz * 1.1f, median));
    }

    void placeMarks()
    {
        for (;;)
        {
            const std::int64_t last = mark (markCount - 1).pos;
            const float period = markPeriod();
            const bool voiced = trackerVoiced;
            const auto candidate = last + (std::int64_t) std::lround (period);
            if (! voiced)
            {
                if (candidate >= now)
                    return;
                pushMark (candidate, false);
                continue;
            }
            const int half = juce::jmax (1, (int) (period * 0.25f));
            const int reach = juce::jmax (1, (int) (period * 0.10f));
            const int span = juce::jmax (4, (int) (period * 0.33f));
            // The refinement below looks `reach + span` further ahead than the peak search.
            if (candidate + half + reach + span + 4 >= now)
                return;
            // Peak of the low-passed signal near the expected position, weighted
            // towards it so the marks keep a steady phase within each period.
            float lo = lowpassed[(size_t) ((candidate - half) & ringMask)];
            for (auto i = candidate - half; i <= candidate + half; ++i)
                lo = std::min (lo, lowpassed[(size_t) (i & ringMask)]);
            auto best = candidate;
            float bestScore = -1.0f;
            for (auto i = candidate - half; i <= candidate + half; ++i)
            {
                const float distance = (float) std::abs ((double) (i - candidate)) / (float) half;
                const float score = (lowpassed[(size_t) (i & ringMask)] - lo) * (1.0f - 0.35f * distance);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
            // (Glottal-closure marks from the LP residual, after SEDREAMS, were tried here and
            // dropped: on breathy voices the residual peaks are noise; see ARCHITECTURE.md section 8.)
            if (mark (markCount - 1).voiced)
                best = refineMark (last, best, reach, span);
            pushMark (juce::jmax (last + juce::jmax (2, (int) (period * 0.5f)), best), true);
        }
    }

    /**
        Move a mark to where the waveform best repeats the one before it: normalised
        cross-correlation of the low-passed signal around the previous mark and around each
        candidate within +-reach. A mild preference for the peak position keeps marks from
        wandering. Breathy and low voices have flat, noisy peaks; the waveform shape is far
        more reliable than the peak, so the periods stop jittering.
    */
    std::int64_t refineMark (std::int64_t last, std::int64_t best, int reach, int span) const
    {
        const int stride = juce::jlimit (1, 8, juce::jmin ((int) (fs / 8000.0), span / 6));
        float refEnergy = 0.0f;
        for (int k = -span; k <= span; k += stride)
        {
            const float v = lowpassed[(size_t) ((last + k) & ringMask)];
            refEnergy += v * v;
        }
        if (refEnergy < 1.0e-10f)
            return best;
        auto winner = best;
        float winnerScore = -2.0f;
        for (auto q = best - reach; q <= best + reach; ++q)
        {
            float cross = 0.0f, energy = 0.0f;
            for (int k = -span; k <= span; k += stride)
            {
                const float a = lowpassed[(size_t) ((last + k) & ringMask)];
                const float b = lowpassed[(size_t) ((q + k) & ringMask)];
                cross += a * b;
                energy += b * b;
            }
            const float score = cross / std::sqrt (refEnergy * energy + 1.0e-20f)
                                * (1.0f - 0.25f * (float) std::abs (q - best) / (float) reach);
            if (score > winnerScore)
            {
                winnerScore = score;
                winner = q;
            }
        }
        return winner;
    }

    /** 0 at a voiced/unvoiced edge rising to 1 over ~5 ms (at least one grain) into the voiced side. */
    float edgeFade (std::int64_t j, double grainLength) const
    {
        const auto& m = mark (j);
        const double edge = std::max (0.005 * fs, 1.5 * grainLength);
        double nearest = edge;
        for (int k = 1; k <= 8; ++k)
        {
            if (j - k >= oldestMark())
            {
                const auto& o = mark (j - k);
                const double d = (double) (m.pos - o.pos);
                if (d >= edge) break;
                if (o.voiced != m.voiced) { nearest = std::min (nearest, d); break; }
            }
            else break;
        }
        for (int k = 1; k <= 8; ++k)
        {
            if (j + k >= markCount) break;
            const auto& o = mark (j + k);
            const double d = (double) (o.pos - m.pos);
            if (d >= edge) break;
            if (o.voiced != m.voiced) { nearest = std::min (nearest, d); break; }
        }
        return smoothstep (0.0f, 1.0f, (float) (nearest / edge));
    }

    // ------------------------------------------------------------------ correction
    float centsOf (float hz) const
    {
        return 6900.0f + 1200.0f * std::log2 (hz / params->referenceHz);
    }

    int nearestAllowed (float cents, NoteMask mask) const
    {
        if (mask == 0)
            return -1;
        const int centre = (int) std::lround (cents / 100.0f);
        int best = -1;
        float bestDistance = 1.0e9f;
        for (int n = centre - 7; n <= centre + 7; ++n)
        {
            if (n < 0 || n > 127 || (mask & (1u << (n % 12))) == 0)
                continue;
            const float d = std::abs (cents - (float) n * 100.0f);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = n;
            }
        }
        return best;
    }

    void decideCorrection (std::int64_t k)
    {
        Frame& f = frame (k);
        const auto& p = *params;
        const float dt = (float) hop / (float) fs;

        if (! f.voiced)
        {
            const float release = 1.0f - std::exp (-dt / 0.05f);
            correctionPull += (0.0f - correctionPull) * release;
            correction += (0.0f - correction) * release;
            correctionForward += (0.0f - correctionForward) * release;
            currentNote = -1;
            noteAge = 0.0;
            centreValid = false;
            f.kind = 0;
            f.pull = correctionPull;
            f.alpha = release;
            f.fullError = 0.0f;
            if (smoothLag == 0)
            {
                f.shiftCents = 0.0f;
                lastShift = 0.0f;
                pushDisplay ({ 0.0f, 0.0f, 0.0f, -1 });
            }
            return;
        }

        if (f.hz <= 0.0f) // bridged dropout: keep the previous shift
        {
            noteAge += dt;
            f.kind = 1;
            f.pull = correctionPull;
            f.alpha = 0.0f;
            f.fullError = 0.0f;
            f.target = currentNote;
            if (smoothLag == 0)
            {
                f.shiftCents = lastShift;
                pushDisplay ({ 0.0f, 0.0f, correction * p.amount * 0.01f, currentNote });
            }
            return;
        }

        // Neighbourhood median (studio looks ahead), used for note decisions and to
        // reject single-frame outliers.
        float values[16];
        int count = 0;
        const std::int64_t from = juce::jmax (oldestFrame(), k - (lookahead > 0 ? 3 : 2));
        const std::int64_t to = lookahead > 0 ? juce::jmin (frameCount - 1, k + 3) : k;
        for (auto i = from; i <= to && count < 16; ++i)
            if (frame (i).hz > 0.0f)
                values[count++] = centsOf (frame (i).hz);
        std::nth_element (values, values + count / 2, values + count);
        const float median = values[count / 2];


        float measured = centsOf (f.hz);
        if (std::abs (measured - median) > 70.0f)
            measured = median;

        // Choose the target note, with hysteresis so wobbles near a boundary don't flip it.
        // (Deciding on the Kalman centre, or on the pitch minus the tracked vibrato, was tried: the
        // centre lags legato steps and the oscillator soaks up slides; see ARCHITECTURE.md section 8.)
        int target = p.forceNote >= 0 ? p.forceNote : nearestAllowed (median, p.allowed);
        if (p.forceNote < 0 && target >= 0 && currentNote >= 0 && target != currentNote
            && (p.allowed & (1u << (currentNote % 12))) != 0)
        {
            const float toCurrent = std::abs (median - (float) currentNote * 100.0f);
            const float toTarget = std::abs (median - (float) target * 100.0f);
            if (toCurrent < toTarget + 25.0f && toCurrent < 100.0f)
                target = currentNote;
        }
        bool reseed = ! centreValid;
        if (target != currentNote)
        {
            currentNote = target;
            noteAge = 0.0;
            vibratoPhase = 0.0;
            reseed = true;
        }

        // The singer's pitch centre and vibrato (VibratoTracker), restarted on every onset and note
        // change. Vibrato and micro-movement sit around the centre and are left alone. The centre
        // may not stray more than 60 ct plus the vibrato depth from the pitch.
        if (reseed)
        {
            vibratoTracker.reset (median);
            centreValid = true;
        }
        else
            vibratoTracker.update (measured, dt);
        const float reach = maxDeviation + vibratoTracker.depth();
        const float centre = juce::jlimit (measured - reach, measured + reach, vibratoTracker.centre());
        vibratoTracker.x[0] = (double) centre - vibratoTracker.origin;

        // Early in a note the reference is the instantaneous pitch (the centre has not settled
        // and a scoop would otherwise read as vibrato); it blends to the slow centre over
        // the next 120-350 ms, which is also when a real vibrato starts.
        const float centreWeight = smoothstep (0.12f, 0.35f, (float) noteAge);
        const float reference = measured + centreWeight * (centre - measured);
        const float deviation = centreWeight * (measured - centre);

        float desired = 0.0f, fullError = 0.0f;
        if (target >= 0)
        {
            const float targetCents = (float) target * 100.0f;
            desired = targetCents - reference;
            if (p.tolerance > 0.0f && p.forceNote < 0)
            {
                const float zone = 50.0f - 0.45f * p.tolerance;
                desired *= 1.0f - smoothstep (zone, zone + 20.0f, std::abs (median - targetCents));
            }
            fullError = desired;
            // Onset ramp: a scoop or attack keeps its shape while the correction fades in over
            // 20-80 ms (longer with Humanize). Instant retune stays instant.
            if (p.retuneMs > 0.0f)
            {
                const float rampSeconds = (0.020f + 0.060f * p.humanize * 0.01f) * juce::jmin (1.0f, p.retuneMs / 20.0f);
                desired *= smoothstep (0.0f, rampSeconds, (float) noteAge);
            }
        }

        float tau = p.retuneMs * 0.001f;
        if (p.humanize > 0.0f)
            tau += p.humanize * 0.01f * 0.25f * smoothstep (0.12f, 0.52f, (float) noteAge);
        // Retune spring. Two identical one-pole stages in series are exactly a critically damped
        // spring-mass-damper, x'' + 2w x' + w^2 x = w^2 u: the fastest second-order response that
        // never rings. Unlike a single pole it starts with zero slope, so the correction eases in
        // instead of kinking at every new target (that kink is the audible "step" of a hard
        // one-pole retune). Its step response is 1 - (1 + w t) e^(-w t), which reaches 63 % at
        // w t = 2.146; each stage gets tau / 2.146, so Retune speed keeps its meaning (63 % of the
        // way in `retuneMs`). 0 ms stays an instant snap.
        const float stageTau = tau / springTimeScale;
        const float alpha = stageTau <= 0.0005f ? 1.0f : 1.0f - std::exp (-dt / stageTau);
        correctionPull += (desired - correctionPull) * alpha;
        if (smoothLag == 0)
            correction += (correctionPull - correction) * alpha;
        else
            correctionForward += (correctionPull - correctionForward) * alpha;
        // Never correct past the note: a slow correction that built up while the singer was
        // rising on their own (a scoop) is released as soon as they arrive.
        const auto clampToError = [fullError] (float& v)
        {
            if ((fullError > 0.0f && v > fullError) || (fullError < 0.0f && v < fullError))
                v = fullError;
        };
        clampToError (correctionPull);
        clampToError (correction);
        clampToError (correctionForward);
        const float naturalTerm = p.naturalVibrato * 0.01f * deviation * p.amount * 0.01f;

        float vibrato = 0.0f;
        if (p.vibratoDepth > 0.0f && target >= 0)
        {
            const float onset = p.vibratoOnsetMs * 0.001f;
            const float ramp = smoothstep (onset, onset + 0.25f, (float) noteAge);
            vibrato = p.vibratoDepth * ramp * (float) std::sin (vibratoPhase);
            vibratoPhase = std::fmod (vibratoPhase + twoPi * p.vibratoRate * dt, twoPi);
        }
        noteAge += dt;

        f.kind = 2;
        f.pull = correctionPull;
        f.alpha = alpha;
        f.fullError = fullError;
        f.extra = naturalTerm + vibrato + p.transpose * 100.0f;
        f.amount = p.amount * 0.01f;
        f.target = target;
        f.causal = correctionForward;
        f.zeroPhaseWeight = smoothstep (0.12f, 0.35f, (float) noteAge - dt);
        if (smoothLag > 0)
            return; // finishCorrection runs the spring's second stage backwards, smoothLag frames later

        const float correctionOnly = correction * f.amount;
        f.shiftCents = correctionOnly + f.extra;
        lastShift = f.shiftCents;
        pushDisplay ({ f.hz, f.hz * std::exp2 (f.shiftCents / 1200.0f), correctionOnly, target });
    }

    /**
        Studio: the retune spring's second stage, run backwards (zero phase).

        The spring is two identical one-pole stages, H(z) = H1(z)^2, so its magnitude response is
        |H1|^2. Running the second stage backwards in time instead gives H1(z) H1(1/z): exactly the
        same magnitude response (Retune speed smooths just as much) and zero phase, so the
        correction is no longer late: the classic forward-backward filter (filtfilt; Gustafsson,
        "Determining the initial states in forward-backward filtering", IEEE Trans. SP 44(4), 1996).
        In real time the backward pass can only start smoothLag frames ahead: it is initialised
        with the forward stage's value there and run back to frame k, a fixed-lag approximation
        that is exact for retune times well under the look-ahead.

        Attacks keep the lag. The scoop checks showed why the forward spring's lag is wanted at the
        start of a note: together with the onset ramp it lets a scoop through, and a zero-phase
        correction is already there at +30-50 ms (the dip kept fell from 95 % to 51-64 %). So the
        result blends from the forward spring to the zero-phase one over 120-350 ms after an onset
        or note change, the same span over which the reference becomes the slow pitch centre:
        steady notes, drifts and the centre are corrected without lag, attacks as before.

        Never past the note, and never against it: a zero-phase correction anticipates the next
        note, so near a note change it may lean towards a correction of the other sign; it is
        limited to between 0 and the current frame's full error.
    */
    void finishCorrection (std::int64_t k)
    {
        Frame& f = frame (k);
        if (f.kind == 0)
        {
            f.shiftCents = 0.0f;
            lastShift = 0.0f;
            pushDisplay ({ 0.0f, 0.0f, 0.0f, -1 });
            return;
        }
        if (f.kind == 1)
        {
            f.shiftCents = lastShift;
            pushDisplay ({ 0.0f, 0.0f, correction * f.amount, f.target });
            return;
        }
        const auto last = juce::jmin (preparedCount - 1, k + (std::int64_t) smoothLag);
        float y = frame (last).pull;
        for (auto j = last - 1; j >= k; --j)
            y += frame (j).alpha * (frame (j).pull - y);
        y = f.causal + f.zeroPhaseWeight * (y - f.causal);
        if (f.fullError > 0.0f) y = juce::jlimit (0.0f, f.fullError, y);
        else if (f.fullError < 0.0f) y = juce::jlimit (f.fullError, 0.0f, y);
        correction = y;
        const float correctionOnly = y * f.amount;
        f.shiftCents = correctionOnly + f.extra;
        lastShift = f.shiftCents;
        pushDisplay ({ f.hz, f.hz * std::exp2 (f.shiftCents / 1200.0f), correctionOnly, f.target });
    }

    void pushDisplay (const DisplayFrame& d)
    {
        int start1, size1, start2, size2;
        displayFifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 > 0)
            displayFrames[(size_t) start1] = d;
        displayFifo.finishedWrite (size1);
    }

    /** Shift in cents for the grain sourced at input time t (from decided frames). */
    float shiftAt (std::int64_t t)
    {
        if (correctedCount == 0)
            return 0.0f;
        std::int64_t lo = oldestFrame(), hi = correctedCount - 1;
        if (lo > hi)
            return 0.0f;
        if (t <= frame (lo).time) return frame (lo).shiftCents;
        if (t >= frame (hi).time)
        {
            // Live mode reads grains newer than the last analysed frame: continue the
            // recent trend of the correction for up to 15 ms instead of holding it.
            const auto& last = frame (hi);
            if (lookahead == 0 && hi - 2 >= lo && last.voiced && last.hz > 0.0f)
            {
                const auto& before = frame (hi - 2);
                if (before.voiced && before.hz > 0.0f && last.time > before.time)
                {
                    const float slope = (last.shiftCents - before.shiftCents) / (float) (last.time - before.time);
                    const float ahead = (float) juce::jmin<std::int64_t> (t - last.time, (std::int64_t) (0.015 * fs));
                    return last.shiftCents + juce::jlimit (-40.0f, 40.0f, slope * ahead);
                }
            }
            return last.shiftCents;
        }
        while (hi - lo > 1)
        {
            const auto mid = (lo + hi) / 2;
            (frame (mid).time <= t ? lo : hi) = mid;
        }
        const auto& a = frame (lo);
        const auto& b = frame (hi);
        const float span = (float) juce::jmax<std::int64_t> (1, b.time - a.time);
        const float w = (float) (t - a.time) / span;
        if (! a.voiced || ! b.voiced)
            return w < 0.5f ? a.shiftCents : b.shiftCents;
        return a.shiftCents + (b.shiftCents - a.shiftCents) * w;
    }

    // ------------------------------------------------------------------ synthesis
    std::int64_t nearestMark (double u) const
    {
        std::int64_t lo = oldestMark(), hi = markCount - 1;
        if (u <= (double) mark (lo).pos) return lo;
        if (u >= (double) mark (hi).pos) return hi;
        while (hi - lo > 1)
        {
            const auto mid = (lo + hi) / 2;
            ((double) mark (mid).pos <= u ? lo : hi) = mid;
        }
        return (u - (double) mark (lo).pos) <= ((double) mark (hi).pos - u) ? lo : hi;
    }

    struct Grain
    {
        std::int64_t centre = 0;
        double left = 0, right = 0;   // analysis half-lengths (samples)
        double ratio = 1.0, formant = 1.0;
        double sourceReach = 0;       // how far right of centre the source is read
    };

    Grain grainFor (std::int64_t j)
    {
        const auto& p = *params;
        Grain g;
        const auto& m = mark (j);
        g.centre = m.pos;
        g.right = (double) (mark (j + 1).pos - m.pos);
        g.left = j > oldestMark() ? (double) (m.pos - mark (j - 1).pos) : g.right;
        const double maxHalf = (double) maxWindow;
        g.left = juce::jlimit (2.0, maxHalf, g.left);
        g.right = juce::jlimit (2.0, maxHalf, g.right);
        const float shift = m.voiced ? juce::jlimit (-2400.0f, 2400.0f, shiftAt (m.pos)) * edgeFade (j, g.right) : 0.0f;
        g.ratio = std::exp2 ((double) shift / 1200.0);
        const double character = std::exp2 ((double) p.formantShift / 12.0) * (100.0 / juce::jlimit (50.0, 200.0, (double) p.throat));
        // The LPC methods never resample grains: PSOLA keeps the envelope, and any envelope move is
        // done by the LPC filters on the output.
        g.formant = method != FormantMethod::classic ? 1.0
                                                     : juce::jlimit (0.5, 2.0, (p.preserveFormants ? 1.0 : g.ratio) * character);
        g.sourceReach = g.right * juce::jmax (1.0, g.formant);
        return g;
    }

    bool sourceAvailable (std::int64_t j, const Grain& g) const
    {
        const auto known = method == FormantMethod::lpcSourceFilter ? residualPos : now;
        return j + 1 < markCount && (double) g.centre + g.sourceReach + 3.0 < (double) known;
    }

    /** True when every input sample in [lo, hi] is known to be digitally silent. */
    bool sourceSilent (std::int64_t lo, std::int64_t hi) const
    {
        const auto completeBlocks = now >> silenceBlockShift;
        for (auto blk = juce::jmax<std::int64_t> (0, lo) >> silenceBlockShift; blk <= (hi >> silenceBlockShift); ++blk)
            if (blk >= completeBlocks || blockLoud[(size_t) (blk & (std::int64_t) (blockLoud.size() - 1))] != 0)
                return false;
        return true;
    }

    void addGrain (double centre, const Grain& g, std::int64_t outputStart)
    {
        // Output half-lengths: grains are stretched (never shortened) so overlap stays >= 50 %.
        const double stretch = g.formant < 1.0 ? 1.0 / g.formant : 1.0;
        const double outLeft = g.left * stretch, outRight = g.right * stretch;
        const auto first = juce::jmax (outputStart, (std::int64_t) std::ceil (centre - outLeft));
        const auto last = (std::int64_t) std::floor (centre + outRight);
        if (first > last)
            return;

        // A grain cut from silence adds nothing (its sum is zero and its weight only matters
        // where the sum is not), so skip the whole loop.
        const double readLo = (double) g.centre + ((double) first - centre) * g.formant;
        const double readHi = (double) g.centre + ((double) last - centre) * g.formant;
        if (sourceSilent ((std::int64_t) std::floor (juce::jmin (readLo, readHi)) - 2, (std::int64_t) std::ceil (juce::jmax (readLo, readHi)) + 3))
            return;

        const auto& table = hannTable();
        const int channels = stereo ? 2 : 1;
        // The LPC re-colouring must use the envelope of the input this output sample was cut from.
        // With unresampled grains that is a constant delay per grain; the weighted mean over the
        // overlapping grains follows Live catch-ups and PSOLA's repeated or skipped periods exactly.
        const bool trackDelay = method != FormantMethod::classic;
        const float grainDelay = (float) (centre - (double) g.centre);
        const bool fixedPhase = g.formant == 1.0; // the read offset is the same for every sample
        float h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        std::int64_t base0 = 0;
        bool direct = false;
        if (fixedPhase)
        {
            const double position = (double) g.centre + ((double) first - centre);
            base0 = (std::int64_t) std::floor (position);
            const float t = (float) (position - (double) base0);
            direct = t < 1.0e-6f; // integral reads need no interpolation
            h0 = t * (-0.5f + t * (1.0f - 0.5f * t));
            h1 = 1.0f + t * t * (-2.5f + 1.5f * t);
            h2 = t * (0.5f + t * (2.0f - 1.5f * t));
            h3 = t * t * (-0.5f + 0.5f * t);
        }

        for (auto i = first; i <= last; ++i)
        {
            const double d = (double) i - centre;
            const double half = d < 0.0 ? outLeft : outRight;
            const float x = (float) (std::abs (d) / half * hannTableSize);
            const int k = juce::jmin (hannTableSize, (int) x);
            const float w = table[(size_t) k] + (table[(size_t) k + 1] - table[(size_t) k]) * (x - (float) k);
            if (w <= 0.0f)
                continue;
            // Every channel is cut at the same marks with the same window, so the stereo image
            // (level, phase and timing differences between the channels) is kept.
            for (int ch = 0; ch < channels; ++ch)
            {
                const bool residual = method == FormantMethod::lpcSourceFilter;
                const auto& ring = ch == 0 ? (residual ? resL : dryL) : (residual ? resR : dryR);
                float sample;
                if (fixedPhase)
                {
                    const auto p = base0 + (i - first);
                    sample = direct ? sampleAt (ring, p)
                                    : h0 * sampleAt (ring, p - 1) + h1 * sampleAt (ring, p) + h2 * sampleAt (ring, p + 1) + h3 * sampleAt (ring, p + 2);
                }
                else
                    sample = interpolatedAt (ring, (double) g.centre + d * g.formant);
                (ch == 0 ? accL : accR)[(size_t) (i & ringMask)] += w * sample;
            }
            win[(size_t) (i & ringMask)] += w;
            if (trackDelay)
                delayAcc[(size_t) (i & ringMask)] += w * grainDelay;
        }
    }

    void scheduleGrains (std::int64_t blockStart, std::int64_t blockEnd)
    {
        if (synthPos < (double) blockStart)
            synthPos = (double) blockStart;

        for (int guard = 0; guard < 4096; ++guard)
        {
            if (markCount < 3)
                return;
            const double u = synthPos - (double) latency - extraDelay;
            auto j = juce::jlimit (oldestMark() + 1, markCount - 2, nearestMark (u));
            auto g = grainFor (j);
            const double stretch = g.formant < 1.0 ? 1.0 / g.formant : 1.0;
            if (synthPos - g.left * stretch >= (double) blockEnd)
                return;

            if (! sourceAvailable (j, g))
            {
                // Not enough input yet: fall back to the newest usable mark and
                // grow the delay so later grains map onto the same timeline.
                while (j > oldestMark() + 1 && ! sourceAvailable (j, g))
                    g = grainFor (--j);
                // Step back one more period of headroom so the next grains don't
                // hit the edge again (each catch-up repeats one period).
                if (j > oldestMark() + 1)
                    g = grainFor (--j);
                if (! sourceAvailable (j, g))
                    return;
                extraDelay = juce::jmax (0.0, synthPos - (double) latency - (double) g.centre);
                delayFloor = juce::jmax (delayFloor, extraDelay);
            }
            else if (extraDelay > 0.0 && ! mark (j).voiced)
            {
                // Give delay back only in unvoiced sound and pauses, where moving the read
                // position is inaudible, and never below what this voice has needed lately.
                // Doing it during a note stretches the audio (a steady detune of several cents)
                // and was the main source of Live-mode damage.
                const double known = (double) (method == FormantMethod::lpcSourceFilter ? residualPos : now);
                const double headroom = known - ((double) g.centre + g.sourceReach);
                if (headroom > 2.5 * g.right)
                    extraDelay = juce::jmax (delayFloor, extraDelay - 0.25 * g.right);
            }
            delayFloor *= std::exp (-g.right / (4.0 * fs));

            addGrain (synthPos, g, blockStart);
            synthPos += juce::jmax (2.0, g.right / juce::jlimit (0.25, 4.0, g.ratio));
        }
    }

    // ------------------------------------------------------------------ block
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const SnaplineParams& p)
    {
        params = &p;
        stereo = inR != nullptr;
        // The envelope method analyses only while the envelope is meant to move; starting it again
        // when a control moves is a method change as far as the LPC history is concerned.
        const bool envelopeStarts = p.formantMethod == FormantMethod::lpcEnvelope && method == p.formantMethod
                                    && envelopeMoves (p) != lpcAnalysing;
        if (mode == Mode::studio && p.mode == mode && studioLookahead() != lookahead)
            applyMode (mode); // the input type sets how far Studio can look ahead
        if (p.mode != mode || p.formantMethod != method || envelopeStarts)
        {
            const bool modeChanged = p.mode != mode;
            method = p.formantMethod;
            if (modeChanged)
            {
                applyMode (p.mode);
                extraDelay = 0.0;
                delayFloor = 0.0;
            }
            restartLpc();
        }

        const std::int64_t blockStart = now;
        for (int i = 0; i < numSamples; ++i)
        {
            const float l = inL[i];
            const float r = inR != nullptr ? inR[i] : l;
            const float mono = inR != nullptr ? 0.5f * (l + r) : l;
            const auto idx = (size_t) (now & ringMask);
            input[idx] = mono;
            lp1 += lpCoeff * (mono - lp1);
            lp2 += lpCoeff * (lp1 - lp2);
            lowpassed[idx] = lp2;
            dryL[idx] = l;
            dryR[idx] = r;
            blockPeak = std::max ({ blockPeak, std::abs (l), std::abs (r) });
            if ((now & ((1 << silenceBlockShift) - 1)) == (1 << silenceBlockShift) - 1)
            {
                blockLoud[(size_t) ((now >> silenceBlockShift) & (std::int64_t) (blockLoud.size() - 1))] = blockPeak > silenceLevel ? 1 : 0;
                blockPeak = 0.0f;
            }
            ++now;
            if (++hopCounter == hop)
            {
                hopCounter = 0;
                analyseFrame();
            }
        }
        placeMarks();
        if (method == FormantMethod::lpcSourceFilter)
            advanceResidual();
        // Mix at zero or bypass engaged (and settled): the wet signal is not heard, so skip the resynthesis.
        // Tracking continues for the display; grains resume from the next block.
        const bool wetHeard = (p.mix > 0.0f || mixSmoothed > 1.0e-4f) && (! p.bypass || bypassSmoothed < 1.0f - 1.0e-4f);
        if (wetHeard)
            scheduleGrains (blockStart, now);

        // LPC re-colouring of the wet signal. The envelope filter of the envelope method runs only
        // while the envelope is meant to move; otherwise PSOLA alone already keeps it.
        const bool wantEnvelope = method == FormantMethod::lpcEnvelope && envelopeMoves (p);
        if (wantEnvelope != envelopeFilterOn || (wetHeard && ! lastWetHeard))
        {
            outInvL.reset(); outInvR.reset(); outSynL.reset(); outSynR.reset();
        }
        envelopeFilterOn = wantEnvelope;
        lastWetHeard = wetHeard;
        const bool recolour = wetHeard && (envelopeFilterOn || method == FormantMethod::lpcSourceFilter);
        const float nominalDelay = (float) ((double) latency + extraDelay);
        const float guardCoeff = 1.0f - std::exp (-1.0f / (0.003f * (float) fs));
        std::int64_t coefficientGrid = std::numeric_limits<std::int64_t>::min();
        effectiveDelay.store ((float) (latency + extraDelay), std::memory_order_relaxed);

        const float mixTarget = juce::jlimit (0.0f, 1.0f, p.mix * 0.01f);
        const float gainTarget = juce::Decibels::decibelsToGain (p.outputDb);
        const float smooth = 1.0f - std::exp (-1.0f / (0.02f * (float) fs));
        const float bypassSmooth = 1.0f - std::exp (-1.0f / (0.004f * (float) fs)); // about 12 ms to settle
        const float bypassTarget = p.bypass ? 1.0f : 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const std::int64_t o = blockStart + i;
            const auto idx = (size_t) (o & ringMask);
            const float wsum = win[idx];
            const float norm = wsum > 0.0f ? 1.0f / juce::jmax (wsum, 0.5f) : 0.0f;
            float wetL = accL[idx] * norm;
            float wetR = stereo ? accR[idx] * norm : wetL;
            if (recolour)
            {
                // Output sample o carries the input from u = o - delay. Source-filter:
                // y = g' e / A'_u. Envelope: y = g' (A_u y_psola) / A'_u, the PSOLA output whitened
                // by its own envelope and re-coloured by the warped one.
                const float d = wsum > 0.0f ? delayAcc[idx] / wsum : nominalDelay;
                const auto u = o - (std::int64_t) std::lround (d);
                constexpr std::int64_t gridMask = (1 << lpcGridShift) - 1;
                const auto grid = u & ~gridMask;
                if (grid != coefficientGrid)
                {
                    coefficientGrid = grid;
                    if (envelopeFilterOn)
                        sourceCoefficients (grid, kSrcOut.data());
                    targetCoefficients (grid, kWarpOut.data(), gainOut);
                }
                if (envelopeFilterOn)
                {
                    wetL = outInvL.process (wetL, kSrcOut.data(), lpcOrder);
                    if (stereo) wetR = outInvR.process (wetR, kSrcOut.data(), lpcOrder);
                }
                wetL = gainOut * outSynL.process (wetL, kWarpOut.data(), lpcOrder);
                wetR = stereo ? gainOut * outSynR.process (wetR, kWarpOut.data(), lpcOrder) : wetL;

                // Guard. Whitening and re-colouring are exact inverses only when a grain is re-coloured
                // with the envelope it was whitened with. Where grains from both sides of a sudden
                // envelope change overlap (the first onset after a stream starts, a voice entering from
                // digital silence) a sharp resonance can briefly add gain. PSOLA keeps the short-term
                // power of its source, so the output may not run more than 6 dB over the input it was
                // cut from; one gain for both channels keeps the image. Inactive in normal operation.
                const auto src = (size_t) (u & ringMask);
                const float dryPower = u >= 0 ? juce::jmax (dryL[src] * dryL[src], dryR[src] * dryR[src]) : 0.0f;
                const float wetPower = juce::jmax (wetL * wetL, wetR * wetR);
                guardDry += guardCoeff * (dryPower - guardDry);
                guardWet += guardCoeff * (wetPower - guardWet);
                const float allowed = 4.0f * guardDry + 1.0e-12f;
                if (guardWet > allowed)
                {
                    const float g = std::sqrt (allowed / guardWet);
                    wetL *= g;
                    wetR *= g;
                }
            }
            accL[idx] = 0.0f;
            accR[idx] = 0.0f;
            win[idx] = 0.0f;
            delayAcc[idx] = 0.0f;
            const std::int64_t dryPos = o - latency;
            const float dl = dryPos >= 0 ? dryL[(size_t) (dryPos & ringMask)] : 0.0f;
            const float dr = dryPos >= 0 ? dryR[(size_t) (dryPos & ringMask)] : 0.0f;
            mixSmoothed += smooth * (mixTarget - mixSmoothed);
            gainSmoothed += smooth * (gainTarget - gainSmoothed);
            bypassSmoothed += bypassSmooth * (bypassTarget - bypassSmoothed);
            // The dry path is delayed by the reported latency, so bypassing never shifts timing.
            const float processedL = (wetL * mixSmoothed + dl * (1.0f - mixSmoothed)) * gainSmoothed;
            outL[i] = processedL + bypassSmoothed * (dl - processedL);
            if (outR != nullptr)
            {
                const float processedR = (wetR * mixSmoothed + dr * (1.0f - mixSmoothed)) * gainSmoothed;
                outR[i] = processedR + bypassSmoothed * (dr - processedR);
            }
        }
        params = nullptr;
    }
};

SnaplineDsp::SnaplineDsp() : impl (std::make_unique<Impl>()) {}
SnaplineDsp::~SnaplineDsp() = default;

void SnaplineDsp::prepare (double sampleRate, int, Mode initialMode)
{
    SnaplineParams defaults;
    impl->params = &defaults;
    impl->prepare (sampleRate, initialMode);
    impl->params = nullptr;
}

void SnaplineDsp::reset()
{
    impl->reset();
}

int SnaplineDsp::getLatencySamples (Mode mode) const { return impl->latencyFor (mode); }
Mode SnaplineDsp::getMode() const { return impl->mode; }
double SnaplineDsp::getFrameRate() const { return impl->fs / (double) impl->hop; }
float SnaplineDsp::getEffectiveDelaySamples() const { return impl->effectiveDelay.load (std::memory_order_relaxed); }

void SnaplineDsp::process (const float* inL, const float* inR, float* outL, float* outR,
                           int numSamples, const SnaplineParams& params)
{
    impl->process (inL, inR, outL, outR, numSamples, params);
}

int SnaplineDsp::popDisplayFrames (DisplayFrame* dest, int maxFrames)
{
    int start1, size1, start2, size2;
    impl->displayFifo.prepareToRead (maxFrames, start1, size1, start2, size2);
    for (int i = 0; i < size1; ++i)
        dest[i] = impl->displayFrames[(size_t) (start1 + i)];
    for (int i = 0; i < size2; ++i)
        dest[size1 + i] = impl->displayFrames[(size_t) (start2 + i)];
    impl->displayFifo.finishedRead (size1 + size2);
    return size1 + size2;
}
}
