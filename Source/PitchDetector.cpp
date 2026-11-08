#include "PitchDetector.h"
#include <cmath>

namespace snapline
{
namespace
{
constexpr float subMultipleMargin = 0.10f;  // a shorter lag may be this much less periodic and still win
constexpr float subMultipleCeiling = 0.35f; // ... but never if it is not clearly periodic itself
constexpr int maxCandidates = 24;           // local minima of d'(tau) scored per frame
constexpr float priorBeta = 18.0f;          // pYIN threshold prior Beta(2, 18)
constexpr float continuityCents = 300.0f;   // width of the continuity prior around the previous pitch
constexpr float continuityFloor = 0.15f;    // a far candidate keeps this much weight (real leaps still win)
}

float PitchDetector::thresholdCdf (float s) const
{
    const float x = juce::jlimit (0.0f, 1.0f, s) * (float) priorSteps;
    const int k = juce::jmin (priorSteps - 1, (int) x);
    return priorCdf[(size_t) k] + (priorCdf[(size_t) k + 1] - priorCdf[(size_t) k]) * (x - (float) k);
}

void PitchDetector::prepare (double sampleRate, int maxWindow)
{
    fs = sampleRate;
    maxOrder = 1;
    while ((1 << maxOrder) < 2 * maxWindow)
        ++maxOrder;
    ffts.clear();
    ffts.resize ((size_t) maxOrder + 1);
    for (int order = 5; order <= maxOrder; ++order)
        ffts[(size_t) order] = std::make_unique<juce::dsp::FFT> (order);
    a.assign ((size_t) (2 << maxOrder), 0.0f);
    b.assign ((size_t) (2 << maxOrder), 0.0f);
    prefix.assign ((size_t) (2 * maxWindow + 1), 0.0);
    diff.assign ((size_t) (maxWindow + 2), 0.0f);

    // Beta(2, b) has the closed-form CDF  I_x(2, b) = 1 - (1 - x)^b (1 + b x).
    priorCdf.resize ((size_t) priorSteps + 1);
    for (int k = 0; k <= priorSteps; ++k)
    {
        const double x = (double) k / priorSteps;
        priorCdf[(size_t) k] = (float) (1.0 - std::pow (1.0 - x, (double) priorBeta) * (1.0 + priorBeta * x));
    }
}

PitchDetector::Result PitchDetector::detect (const float* frame, int window, int minLag, float threshold, float minRms,
                                             float previousHz)
{
    Result result;
    const int W = window;
    const int n = 2 * W;
    int order = 5;
    while ((1 << order) < n)
        ++order;
    if (order > maxOrder || ffts.empty() || minLag < 2 || minLag >= W - 1)
        return result;
    const int fftSize = 1 << order;
    auto& fft = ffts[(size_t) order];

    prefix[0] = 0.0;
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) frame[i] * (double) frame[i];
    result.rms = (float) std::sqrt (prefix[(size_t) n] / (double) n);
    const double energy0 = prefix[(size_t) W];
    if (energy0 < 1.0e-10 || result.rms < minRms)
        return result;

    // r(tau) = sum_{j<W} x[j] x[j + tau], via IFFT(conj(A) * B). Only the transform's
    // input half needs zero padding.
    std::copy (frame, frame + W, a.begin());
    std::fill (a.begin() + W, a.begin() + fftSize, 0.0f);
    std::copy (frame, frame + n, b.begin());
    std::fill (b.begin() + n, b.begin() + fftSize, 0.0f);
    fft->performRealOnlyForwardTransform (a.data());
    fft->performRealOnlyForwardTransform (b.data());
    for (int k = 0; k < fftSize; ++k)
    {
        const float ar = a[(size_t) (2 * k)], ai = a[(size_t) (2 * k + 1)];
        const float br = b[(size_t) (2 * k)], bi = b[(size_t) (2 * k + 1)];
        b[(size_t) (2 * k)] = ar * br + ai * bi;
        b[(size_t) (2 * k + 1)] = ar * bi - ai * br;
    }
    fft->performRealOnlyInverseTransform (b.data());

    // Calibrate the transform's scaling against the directly computed energy.
    const double r0 = (double) b[0];
    if (std::abs (r0) < 1.0e-20)
        return result;
    const double scale = energy0 / r0;

    // Cumulative mean normalised difference d'(tau).
    diff[0] = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= W; ++tau)
    {
        const double energyTau = prefix[(size_t) (tau + W)] - prefix[(size_t) tau];
        const double d = juce::jmax (0.0, energy0 + energyTau - 2.0 * scale * (double) b[(size_t) tau]);
        running += d;
        diff[(size_t) tau] = running > 0.0 ? (float) (d * (double) tau / running) : 1.0f;
    }

    // pYIN-style candidate scoring (Mauch & Dixon, 2014), adapted for streaming.
    //
    // Plain YIN takes the first dip of d'(tau) under one fixed threshold s. pYIN treats s as
    // uncertain, with a prior P(s) = Beta(2, 18), and asks for each local minimum (tau_i, v_i):
    // for which thresholds is it the first dip found? Walking the minima in lag order with
    // m_i = min(v_0 .. v_{i-1}), minimum i wins exactly for s in (v_i, m_i], so
    //     P(tau_i) = CDF(m_i) - CDF(v_i)   if v_i < m_i, else 0,
    // and the sum over i, CDF(1) - CDF(min v), is the frame's voiced probability. The full pYIN
    // then runs a Viterbi pass over many frames; a real-time tracker cannot wait for that, so the
    // temporal part is collapsed into one step: each candidate is weighted by a continuity prior
    // around the previous voiced pitch (a soft Gaussian in cents with a floor, so a genuine leap
    // with a much better dip still wins). The engine's jump guard and two-frame confirmation then
    // act as the rest of the "HMM".
    int candTau[maxCandidates];
    float candValue[maxCandidates];
    int candidates = 0;
    for (int tau = juce::jmax (minLag, 1); tau < W - 1 && candidates < maxCandidates; ++tau)
    {
        const float v = diff[(size_t) tau];
        if (v < diff[(size_t) tau - 1] && v <= diff[(size_t) tau + 1] && v < 1.0f)
        {
            candTau[candidates] = tau;
            candValue[candidates] = v;
            ++candidates;
        }
    }

    int best = -1;
    float bestScore = 0.0f;
    float voicedProbability = 0.0f;
    float runningMin = 1.0f;
    for (int i = 0; i < candidates; ++i)
    {
        const float v = candValue[i];
        if (v >= runningMin)
            continue;
        const float probability = thresholdCdf (runningMin) - thresholdCdf (v);
        runningMin = v;
        voicedProbability += probability;
        float weight = 1.0f;
        if (previousHz > 0.0f)
        {
            const float cents = 1200.0f * std::log2 ((float) fs / ((float) candTau[i] * previousHz));
            const float z = cents / continuityCents;
            weight = continuityFloor + (1.0f - continuityFloor) * std::exp (-0.5f * z * z);
        }
        // The caller's threshold keeps its YIN meaning: a dip under it is always a candidate
        // worth having, so it is never outscored by a later, deeper dip from the prior alone
        // unless continuity says so.
        const float score = probability * weight + (v < threshold && best < 0 ? 1.0e-3f : 0.0f);
        if (score > bestScore)
        {
            bestScore = score;
            best = candTau[i];
        }
    }
    result.voicedProbability = voicedProbability;

    if (best < 0)
    {
        best = minLag;
        for (int tau = minLag + 1; tau < W; ++tau)
            if (diff[(size_t) tau] < diff[(size_t) best])
                best = tau;
    }

    // Multiple-of-the-period guard. The first dip below the threshold can sit at two, three or more
    // periods when the real period is just above it (breathy or fading notes). If a shorter lag
    // k times smaller is nearly as periodic, that is the real period.
    for (int k = 2; k <= 6; ++k)
    {
        const int centre = (int) std::lround ((double) best / k);
        if (centre < minLag)
            break;
        int local = centre;
        for (int tau = juce::jmax (minLag, centre - 2); tau <= juce::jmin (W - 1, centre + 2); ++tau)
            if (diff[(size_t) tau] < diff[(size_t) local])
                local = tau;
        if (diff[(size_t) local] < diff[(size_t) best] + subMultipleMargin && diff[(size_t) local] < subMultipleCeiling)
        {
            best = local;
            break;
        }
    }

    float refined = (float) best;
    if (best > 1 && best < W)
    {
        const float s0 = diff[(size_t) best - 1], s1 = diff[(size_t) best], s2 = diff[(size_t) best + 1];
        const float denom = s0 - 2.0f * s1 + s2;
        if (std::abs (denom) > 1.0e-9f)
            refined += juce::jlimit (-0.5f, 0.5f, 0.5f * (s0 - s2) / denom);
    }

    result.aperiodicity = diff[(size_t) best];
    result.hz = refined > 0.0f ? (float) (fs / (double) refined) : 0.0f;
    return result;
}
}
