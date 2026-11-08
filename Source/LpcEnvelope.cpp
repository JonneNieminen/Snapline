#include "LpcEnvelope.h"
#include <algorithm>
#include <cmath>

namespace snapline
{
namespace
{
constexpr double pi = 3.141592653589793238463;
constexpr float preEmphasis = 0.97f;
constexpr double lagWindowHz = 60.0;   // Gaussian lag window: envelope resolution, not harmonic detail
constexpr double noiseFloor = 1.0e-4;  // -40 dB white-noise correction on r[0]
constexpr float maxReflection = 0.999f;
}

void LpcAnalyzer::prepare (double sampleRate, int maxFrame)
{
    fs = sampleRate;
    p = fs <= 50000.0 ? 16 : std::min (maxLpcOrder, (int) std::lround (16.0 * fs / 48000.0));
    windowed.assign ((size_t) std::max (1, maxFrame), 0.0f);

    lagWindow.resize ((size_t) maxLpcOrder + 1);
    for (int m = 0; m <= maxLpcOrder; ++m)
    {
        const double x = 2.0 * pi * lagWindowHz * m / fs;
        lagWindow[(size_t) m] = std::exp (-0.5 * x * x);
    }

    cosTable.resize ((size_t) (maxLpcOrder + 1) * (bins + 1));
    sinTable.resize (cosTable.size());
    for (int i = 0; i <= maxLpcOrder; ++i)
        for (int q = 0; q <= bins; ++q)
        {
            const double w = pi * q * i / bins;
            cosTable[(size_t) (i * (bins + 1) + q)] = std::cos (w);
            sinTable[(size_t) (i * (bins + 1) + q)] = std::sin (w);
        }
}

void LpcAnalyzer::solve (std::array<double, maxLpcOrder + 1>& r, float* k, float& error, bool condition) const
{
    // Conditioning: lag window (multiplying r by a Gaussian = convolving the power spectrum with
    // one, which widens every formant a little) and a white-noise floor on r[0]. A warped
    // envelope was already smoothed when it was analysed, so it gets only a tiny floor.
    if (condition)
        for (int m = 0; m <= p; ++m)
            r[(size_t) m] *= lagWindow[(size_t) m];
    r[0] *= 1.0 + (condition ? noiseFloor : 1.0e-7);

    std::fill (k, k + p, 0.0f);
    if (! (r[0] > 1.0e-18))
    {
        error = 0.0f;
        return;
    }

    // Levinson-Durbin. At order m, with predictor a^(m-1) and error E_{m-1}:
    //   k_m      = -(r[m] + sum_{i=1}^{m-1} a_i r[m-i]) / E_{m-1}
    //   a_i^(m)  = a_i^(m-1) + k_m a_{m-i}^(m-1),   a_m^(m) = k_m
    //   E_m      = (1 - k_m^2) E_{m-1}
    std::array<double, maxLpcOrder + 1> a {}, previous {};
    a[0] = 1.0;
    double e = r[0];
    for (int m = 1; m <= p; ++m)
    {
        double acc = r[(size_t) m];
        for (int i = 1; i < m; ++i)
            acc += a[(size_t) i] * r[(size_t) (m - i)];
        double km = -acc / e;
        km = std::clamp (km, -(double) maxReflection, (double) maxReflection);
        previous = a;
        for (int i = 1; i < m; ++i)
            a[(size_t) i] = previous[(size_t) i] + km * previous[(size_t) (m - i)];
        a[(size_t) m] = km;
        e *= 1.0 - km * km;
        k[m - 1] = (float) km;
    }
    error = (float) e;
}

void LpcAnalyzer::analyse (const float* x, int n, float* k, float& error)
{
    n = std::min (n, (int) windowed.size());
    // Hann window and pre-emphasis in one pass. The window's cosine comes from a rotation
    // recurrence (one complex multiply per sample) instead of a cos() call.
    const double step = 2.0 * pi / n;
    const double cs = std::cos (step), sn = std::sin (step);
    double c = std::cos (0.5 * step), s = std::sin (0.5 * step);
    float last = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float w = (float) (0.5 - 0.5 * c);
        const double cNext = c * cs - s * sn;
        s = s * cs + c * sn;
        c = cNext;
        const float emphasised = x[i] - preEmphasis * last;
        last = x[i];
        windowed[(size_t) i] = w * emphasised;
    }

    // Autocorrelation r[m] = sum_i x[i] x[i + m], m = 0..p. The lag loop is innermost: for each i
    // it updates p + 1 independent accumulators from p + 1 contiguous samples, which maps straight
    // onto SIMD lanes (NEON / SSE / AVX2) with no reduction across iterations, so it vectorises
    // without -ffast-math. The last p samples, which lack a full set of partners, are done apart.
    float acc[maxLpcOrder + 1] {};
    const float* w = windowed.data();
    const int order1 = p + 1;
    const int full = n - p;
    for (int i = 0; i < full; ++i)
    {
        const float xi = w[i];
        const float* partner = w + i;
        for (int m = 0; m < order1; ++m)
            acc[m] += xi * partner[m];
    }
    for (int i = std::max (0, full); i < n; ++i)
        for (int m = 0; m < n - i; ++m)
            acc[m] += w[i] * w[i + m];
    std::array<double, maxLpcOrder + 1> r {};
    for (int m = 0; m <= p; ++m)
        r[(size_t) m] = (double) acc[m];
    solve (r, k, error, true);
}

void LpcAnalyzer::warp (const float* k, float error, double phi, float* kOut, float& gain)
{
    if (error <= 0.0f || std::abs (phi - 1.0) < 1.0e-6)
    {
        std::copy (k, k + p, kOut);
        gain = 1.0f;
        return;
    }

    // Step-up recursion: reflection coefficients -> direct-form a_i.
    std::array<double, maxLpcOrder + 1> a {}, previous {};
    a[0] = 1.0;
    for (int m = 1; m <= p; ++m)
    {
        previous = a;
        const double km = k[m - 1];
        for (int i = 1; i < m; ++i)
            a[(size_t) i] = previous[(size_t) i] + km * previous[(size_t) (m - i)];
        a[(size_t) m] = km;
    }

    // Model power spectrum P(w_q) = E / |A(e^{j w_q})|^2 at w_q = pi q / bins.
    for (int q = 0; q <= bins; ++q)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i <= p; ++i)
        {
            re += a[(size_t) i] * cosTable[(size_t) (i * (bins + 1) + q)];
            im -= a[(size_t) i] * sinTable[(size_t) (i * (bins + 1) + q)];
        }
        envelope[(size_t) q] = (double) error / std::max (1.0e-12, re * re + im * im);
    }

    // P'(w) = P(w / phi): the formant at f moves to phi * f.
    for (int q = 0; q <= bins; ++q)
    {
        const double source = std::min ((double) bins, q / phi);
        const int lo = std::min (bins - 1, (int) source);
        const double t = source - lo;
        warped[(size_t) q] = envelope[(size_t) lo] + t * (envelope[(size_t) lo + 1] - envelope[(size_t) lo]);
    }

    // Wiener-Khinchin: r'[m] = (1 / 2 bins) [P'_0 + (-1)^m P'_bins + 2 sum_{q=1}^{bins-1} P'_q cos (pi q m / bins)].
    std::array<double, maxLpcOrder + 1> r {};
    for (int m = 0; m <= p; ++m)
    {
        const double* c = cosTable.data() + (size_t) (m * (bins + 1));
        double acc = warped[0] + ((m & 1) ? -warped[bins] : warped[bins]);
        for (int q = 1; q < bins; ++q)
            acc += 2.0 * warped[(size_t) q] * c[q];
        r[(size_t) m] = acc / (2.0 * bins);
    }
    float warpedError = 0.0f;
    solve (r, kOut, warpedError, false);
    gain = std::clamp ((float) std::sqrt (std::max (0.0f, warpedError) / error), 0.25f, 4.0f);
}
}
