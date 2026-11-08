#include "PitchDetector.h"
#include <cmath>

namespace snapline
{
void PitchDetector::prepare (double sampleRate, int maxWindow)
{
    fs = sampleRate;
    int order = 1;
    while ((1 << order) < 2 * maxWindow)
        ++order;
    fftSize = 1 << order;
    fft = std::make_unique<juce::dsp::FFT> (order);
    a.assign ((size_t) fftSize * 2, 0.0f);
    b.assign ((size_t) fftSize * 2, 0.0f);
    prefix.assign ((size_t) (2 * maxWindow + 1), 0.0);
    diff.assign ((size_t) (maxWindow + 2), 0.0f);
}

PitchDetector::Result PitchDetector::detect (const float* frame, int window, int minLag, float threshold)
{
    Result result;
    const int W = window;
    const int n = 2 * W;
    if (fft == nullptr || n > fftSize || minLag < 2 || minLag >= W - 1)
        return result;

    prefix[0] = 0.0;
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) frame[i] * (double) frame[i];
    result.rms = (float) std::sqrt (prefix[(size_t) n] / (double) n);
    const double energy0 = prefix[(size_t) W];
    if (energy0 < 1.0e-10)
        return result;

    // r(tau) = sum_{j<W} x[j] x[j + tau], via IFFT(conj(A) * B).
    std::fill (a.begin(), a.end(), 0.0f);
    std::fill (b.begin(), b.end(), 0.0f);
    std::copy (frame, frame + W, a.begin());
    std::copy (frame, frame + n, b.begin());
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

    // First dip under the threshold (followed to its local minimum), else the global minimum.
    int best = -1;
    for (int tau = minLag; tau < W; ++tau)
    {
        if (diff[(size_t) tau] < threshold)
        {
            while (tau + 1 < W && diff[(size_t) tau + 1] < diff[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0)
    {
        best = minLag;
        for (int tau = minLag + 1; tau < W; ++tau)
            if (diff[(size_t) tau] < diff[(size_t) best])
                best = tau;
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
