#include "SnaplineDsp.h"
#include "PitchDetector.h"
#include <algorithm>
#include <array>
#include <cmath>
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
constexpr int studioLookaheadFrames = 8;  // about 21 ms at a 375 Hz frame rate
constexpr double framesPerSecond = 375.0;
constexpr float yinThreshold = 0.15f;
constexpr float voicedOnAperiodicity = 0.25f;
constexpr float voicedHoldAperiodicity = 0.35f;
constexpr float gateRms = 0.001f;         // -60 dBFS
constexpr float relativeGate = 0.08f;     // -22 dB below the recent vocal level
constexpr double holdSeconds = 0.03;      // bridge short detection dropouts inside a note
constexpr float centreHz = 3.0f;          // cut-off of the slow pitch centre
constexpr float maxDeviation = 60.0f;     // cents the pitch may stray from its centre before it drags it along
constexpr int markCapacity = 8192;
constexpr int frameCapacity = 4096;
constexpr int displayCapacity = 4096;

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
    };

    double fs = 48000.0;
    int hop = 128;
    int maxWindow = 0;
    int unvoicedPeriod = 256;
    int ringSize = 0, ringMask = 0;

    std::vector<float> input, lowpassed, dryL, dryR, acc, win, scratch;
    std::vector<std::uint8_t> blockLoud;   // per 32-sample block of `input`: any sample above -140 dB
    float blockPeak = 0.0f;
    int hopCounter = 0;
    PitchDetector detector;

    std::int64_t now = 0;                 // samples received
    std::vector<Mark> marks;
    std::int64_t markCount = 0;
    std::vector<Frame> frames;
    std::int64_t frameCount = 0;          // frames analysed
    std::int64_t correctedCount = 0;      // frames with a correction decided

    Mode mode = Mode::studio;
    int latency = 0;
    int lookahead = 0;
    double synthPos = 0.0;                // next grain centre (output timeline)
    double extraDelay = 0.0;              // live-mode adaptive delay on top of `latency`
    float lp1 = 0.0f, lp2 = 0.0f, lpCoeff = 0.0f;

    // Tracker state
    bool trackerVoiced = false;
    float lastVoicedHz = 0.0f;
    float lastShift = 0.0f;
    int holdFramesLeft = 0;
    float recentHz[3] {};
    float levelEnvelope = 0.0f;

    // Correction state
    int currentNote = -1;
    double noteAge = 0.0;
    float correction = 0.0f;
    double vibratoPhase = 0.0;
    float centreA = 0.0f, centreB = 0.0f; // two cascaded 3 Hz poles: the singer's slow pitch centre (cents)
    bool centreValid = false;

    // Output smoothing
    float mixSmoothed = 1.0f, gainSmoothed = 1.0f, bypassSmoothed = 0.0f; // bypassSmoothed: 1 = fully dry

    juce::AbstractFifo displayFifo { displayCapacity };
    std::array<DisplayFrame, displayCapacity> displayFrames {};

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
        for (auto* v : { &input, &lowpassed, &dryL, &dryR, &acc, &win })
            v->assign ((size_t) ringSize, 0.0f);
        scratch.assign ((size_t) (2 * maxWindow + 4), 0.0f);
        blockLoud.assign ((size_t) (ringSize >> silenceBlockShift), (std::uint8_t) 0);
        detector.prepare (fs, maxWindow);
        marks.assign ((size_t) markCapacity, {});
        frames.assign ((size_t) frameCapacity, {});
        lpCoeff = (float) (1.0 - std::exp (-twoPi * 900.0 / fs));
        mode = initialMode;
        reset();
    }

    void reset()
    {
        for (auto* v : { &input, &lowpassed, &dryL, &dryR, &acc, &win })
            std::fill (v->begin(), v->end(), 0.0f);
        now = 0;
        hopCounter = 0;
        blockPeak = 0.0f;
        std::fill (blockLoud.begin(), blockLoud.end(), (std::uint8_t) 0);
        markCount = 0;
        pushMark (0, false);
        frameCount = 0;
        correctedCount = 0;
        lp1 = lp2 = 0.0f;
        trackerVoiced = false;
        lastVoicedHz = 0.0f;
        lastShift = 0.0f;
        holdFramesLeft = 0;
        levelEnvelope = 0.0f;
        std::fill (std::begin (recentHz), std::end (recentHz), 0.0f);
        currentNote = -1;
        noteAge = 0.0;
        correction = 0.0f;
        vibratoPhase = 0.0;
        centreA = centreB = 0.0f;
        centreValid = false;
        bypassSmoothed = 0.0f;
        applyMode (mode);
        synthPos = (double) latency;
        extraDelay = 0.0;
        displayFifo.reset();
    }

    int latencyFor (Mode m) const
    {
        return (int) std::lround (fs * (m == Mode::studio ? studioLatencySeconds : liveLatencySeconds));
    }

    void applyMode (Mode m)
    {
        mode = m;
        latency = latencyFor (m);
        lookahead = m == Mode::studio ? studioLookaheadFrames : 0;
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

    float sampleAt (std::int64_t t) const
    {
        return t < 0 ? 0.0f : input[(size_t) (t & ringMask)];
    }

    float interpolatedAt (double position) const
    {
        const auto base = (std::int64_t) std::floor (position);
        const float frac = (float) (position - (double) base);
        return hermite (sampleAt (base - 1), sampleAt (base), sampleAt (base + 1), sampleAt (base + 2), frac);
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
                                   juce::jmax (gateRms, levelEnvelope * decay * relativeGate));
        }

        const bool inRange = res.hz >= r.minHz * 0.9f && res.hz <= r.maxHz * 1.1f;
        const float limit = trackerVoiced ? voicedHoldAperiodicity : voicedOnAperiodicity;
        // Level relative to the recent vocal: quiet note tails and room noise are
        // not tracked (they invite octave errors), the hold below bridges the edge.
        levelEnvelope = juce::jmax (res.rms, levelEnvelope * decay);
        const bool loudEnough = res.rms > gateRms && res.rms > levelEnvelope * relativeGate;
        bool voiced = inRange && loudEnough && res.aperiodicity < limit;

        // Octave-jump guard: a sudden factor-of-two jump inside a voiced run is
        // almost always a detection error; keep the previous estimate's octave.
        if (voiced && trackerVoiced && lastVoicedHz > 0.0f)
        {
            const float ratio = res.hz / lastVoicedHz;
            if (ratio > 1.85f && ratio < 2.15f) res.hz *= 0.5f;
            else if (ratio > 0.465f && ratio < 0.54f) res.hz *= 2.0f;
        }

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

        // Median of the last three measured periods drives pitch-mark spacing.
        if (f.hz > 0.0f)
        {
            recentHz[0] = recentHz[1];
            recentHz[1] = recentHz[2];
            recentHz[2] = f.hz;
        }

        frame (frameCount) = f;
        ++frameCount;

        while (correctedCount < frameCount - lookahead)
            decideCorrection (correctedCount++);
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
            correction += (0.0f - correction) * (1.0f - std::exp (-dt / 0.05f));
            currentNote = -1;
            noteAge = 0.0;
            centreValid = false;
            f.shiftCents = 0.0f;
            lastShift = 0.0f;
            pushDisplay ({ 0.0f, 0.0f, 0.0f, -1 });
            return;
        }

        if (f.hz <= 0.0f) // bridged dropout: keep the previous shift
        {
            f.shiftCents = lastShift;
            noteAge += dt;
            pushDisplay ({ 0.0f, 0.0f, correction * p.amount * 0.01f, currentNote });
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

        // The singer's slow pitch centre: two 3 Hz poles, restarted on every onset and note
        // change. Vibrato and micro-movement sit around it and are left alone.
        if (reseed)
        {
            centreA = centreB = median;
            centreValid = true;
        }
        else
        {
            const float a = 1.0f - std::exp (-(float) twoPi * centreHz * dt);
            centreA += a * (measured - centreA);
            centreB += a * (centreA - centreB);
            centreA = juce::jlimit (measured - maxDeviation, measured + maxDeviation, centreA);
            centreB = juce::jlimit (measured - maxDeviation, measured + maxDeviation, centreB);
        }

        // Early in a note the reference is the instantaneous pitch (the centre has not settled
        // and a scoop would otherwise read as vibrato); it blends to the slow centre over
        // the next 120-350 ms, which is also when a real vibrato starts.
        const float centreWeight = smoothstep (0.12f, 0.35f, (float) noteAge);
        const float reference = measured + centreWeight * (centreB - measured);
        const float deviation = centreWeight * (measured - centreB);

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
        const float alpha = tau <= 0.0005f ? 1.0f : 1.0f - std::exp (-dt / tau);
        correction += (desired - correction) * alpha;
        // Never correct past the note: a slow correction that built up while the singer was
        // rising on their own (a scoop) is released as soon as they arrive.
        if ((fullError > 0.0f && correction > fullError) || (fullError < 0.0f && correction < fullError))
            correction = fullError;
        const float correctionOnly = correction * p.amount * 0.01f;
        const float applied = correctionOnly + p.naturalVibrato * 0.01f * deviation * p.amount * 0.01f;

        float vibrato = 0.0f;
        if (p.vibratoDepth > 0.0f && target >= 0)
        {
            const float onset = p.vibratoOnsetMs * 0.001f;
            const float ramp = smoothstep (onset, onset + 0.25f, (float) noteAge);
            vibrato = p.vibratoDepth * ramp * (float) std::sin (vibratoPhase);
            vibratoPhase = std::fmod (vibratoPhase + twoPi * p.vibratoRate * dt, twoPi);
        }

        f.shiftCents = applied + vibrato + p.transpose * 100.0f;
        lastShift = f.shiftCents;
        noteAge += dt;
        pushDisplay ({ f.hz, f.hz * std::exp2 (f.shiftCents / 1200.0f), correctionOnly, target });
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
        g.formant = juce::jlimit (0.5, 2.0, (p.preserveFormants ? 1.0 : g.ratio) * character);
        g.sourceReach = g.right * juce::jmax (1.0, g.formant);
        return g;
    }

    bool sourceAvailable (std::int64_t j, const Grain& g) const
    {
        return j + 1 < markCount && (double) g.centre + g.sourceReach + 3.0 < (double) now;
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
            float sample;
            if (fixedPhase)
            {
                const auto p = base0 + (i - first);
                sample = direct ? sampleAt (p)
                                : h0 * sampleAt (p - 1) + h1 * sampleAt (p) + h2 * sampleAt (p + 1) + h3 * sampleAt (p + 2);
            }
            else
                sample = interpolatedAt ((double) g.centre + d * g.formant);
            acc[(size_t) (i & ringMask)] += w * sample;
            win[(size_t) (i & ringMask)] += w;
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
            }
            else if (extraDelay > 0.0)
            {
                // Give delay back slowly once there is plenty of input in hand.
                const double headroom = (double) now - ((double) g.centre + g.sourceReach);
                if (headroom > 2.5 * g.right)
                    extraDelay = juce::jmax (0.0, extraDelay - 0.005 * g.right);
            }

            addGrain (synthPos, g, blockStart);
            synthPos += juce::jmax (2.0, g.right / juce::jlimit (0.25, 4.0, g.ratio));
        }
    }

    // ------------------------------------------------------------------ block
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const SnaplineParams& p)
    {
        params = &p;
        if (p.mode != mode)
        {
            applyMode (p.mode);
            extraDelay = 0.0;
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
            blockPeak = std::max (blockPeak, std::abs (mono));
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
        // Mix at zero or bypass engaged (and settled): the wet signal is not heard, so skip the resynthesis.
        // Tracking continues for the display; grains resume from the next block.
        const bool wetHeard = (p.mix > 0.0f || mixSmoothed > 1.0e-4f) && (! p.bypass || bypassSmoothed < 1.0f - 1.0e-4f);
        if (wetHeard)
            scheduleGrains (blockStart, now);
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
            const float wet = wsum > 0.0f ? acc[idx] / juce::jmax (wsum, 0.5f) : 0.0f;
            acc[idx] = 0.0f;
            win[idx] = 0.0f;
            const std::int64_t dryPos = o - latency;
            const float dl = dryPos >= 0 ? dryL[(size_t) (dryPos & ringMask)] : 0.0f;
            const float dr = dryPos >= 0 ? dryR[(size_t) (dryPos & ringMask)] : 0.0f;
            mixSmoothed += smooth * (mixTarget - mixSmoothed);
            gainSmoothed += smooth * (gainTarget - gainSmoothed);
            bypassSmoothed += bypassSmooth * (bypassTarget - bypassSmoothed);
            // The dry path is delayed by the reported latency, so bypassing never shifts timing.
            const float processedL = (wet * mixSmoothed + dl * (1.0f - mixSmoothed)) * gainSmoothed;
            outL[i] = processedL + bypassSmoothed * (dl - processedL);
            if (outR != nullptr)
            {
                const float processedR = (wet * mixSmoothed + dr * (1.0f - mixSmoothed)) * gainSmoothed;
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
