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

    // FFT for the discrete all-pole and true-envelope fits: at least twice the longest frame.
    int order = 9;
    while ((1 << order) < 2 * std::max (1, maxFrame))
        ++order;
    fft = std::make_unique<juce::dsp::FFT> (order);
    fftSize = 1 << order;
    spectrum.assign ((size_t) (2 * fftSize), 0.0f);
    cepstrum.assign ((size_t) (2 * fftSize), 0.0f);
    power.assign ((size_t) (fftSize / 2 + 1), 0.0);
    logAmplitude.assign (power.size(), 0.0);
    smooth.assign (power.size(), 0.0);
    dapOmega.assign (maxHarmonics, 0.0);
    dapPower.assign (maxHarmonics, 0.0);
    dapCos.assign ((size_t) maxHarmonics * (maxLpcOrder + 1), 0.0);
    dapSin.assign (dapCos.size(), 0.0);

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

int LpcAnalyzer::powerSpectrum (const float* x, int n)
{
    n = std::min ({ n, (int) windowed.size(), fftSize });
    std::fill (spectrum.begin(), spectrum.end(), 0.0f);
    float last = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (i + 0.5) / n));
        spectrum[(size_t) i] = w * (x[i] - preEmphasis * last);
        last = x[i];
    }
    fft->performRealOnlyForwardTransform (spectrum.data());
    const int half = fftSize / 2;
    for (int b = 0; b <= half; ++b)
    {
        const double re = spectrum[(size_t) (2 * b)], im = spectrum[(size_t) (2 * b + 1)];
        power[(size_t) b] = re * re + im * im;
    }
    return half + 1;
}

namespace
{
/** Reflection coefficients from direct form (step-down); false if the filter is not minimum phase. */
bool stepDown (std::array<double, maxLpcOrder + 1> a, int p, float* k)
{
    for (int m = p; m >= 1; --m)
    {
        const double km = a[(size_t) m];
        if (! (std::abs (km) < 0.999))
            return false;
        k[m - 1] = (float) km;
        std::array<double, maxLpcOrder + 1> previous = a;
        for (int i = 1; i < m; ++i)
            a[(size_t) i] = (previous[(size_t) i] - km * previous[(size_t) (m - i)]) / (1.0 - km * km);
    }
    return true;
}
}

void LpcAnalyzer::analyseDap (const float* x, int n, double f0, float* k, float& error)
{
    const int count = powerSpectrum (x, n);
    const double binHz = fs / fftSize;
    double* omega = dapOmega.data();
    double* P = dapPower.data();
    int M = 0;
    double strongest = 0.0;
    for (int m = 1; m < maxHarmonics && m * f0 < 0.5 * fs - 0.5 * f0; ++m)
    {
        const double centre = m * f0 / binHz;
        int b = (int) std::lround (centre);
        for (int c = (int) std::floor (centre) - 2; c <= (int) std::ceil (centre) + 2; ++c)
            if (c > 0 && c < count - 1 && power[(size_t) c] > power[(size_t) b]) b = c;
        if (b <= 0 || b >= count - 1)
            break;
        // Parabolic refinement of the peak in log power.
        const double l0 = std::log (power[(size_t) b - 1] + 1.0e-30), l1 = std::log (power[(size_t) b] + 1.0e-30), l2 = std::log (power[(size_t) b + 1] + 1.0e-30);
        const double d = l0 - 2.0 * l1 + l2;
        const double shift = std::abs (d) > 1.0e-12 ? std::clamp (0.5 * (l0 - l2) / d, -0.5, 0.5) : 0.0;
        omega[M] = 2.0 * pi * (b + shift) / fftSize;
        P[M] = std::exp (l1 - 0.25 * (l0 - l2) * shift);
        strongest = std::max (strongest, P[M]);
        ++M;
    }
    int kept = 0;
    for (int m = 0; m < M; ++m)
        if (P[m] > 1.0e-9 * strongest)
        {
            omega[kept] = omega[m];
            P[kept] = P[m];
            ++kept;
        }
    M = kept;
    // DAP is determined only by at least q + 1 harmonics: a very high voice gets a lower order
    // (the remaining reflection coefficients are zero), not a model fitted to nothing.
    const int q = std::min (p, M - 1);
    if (q < 4 || strongest <= 0.0)
    {
        analyse (x, n, k, error);
        return;
    }
    std::fill (k, k + p, 0.0f);
    // cos / sin (i w_m) once per frame (tables allocated in prepare()).
    const auto cs = [this] (int m, int i) -> double& { return dapCos[(size_t) (m * (maxLpcOrder + 1) + i)]; };
    const auto sn = [this] (int m, int i) -> double& { return dapSin[(size_t) (m * (maxLpcOrder + 1) + i)]; };
    for (int m = 0; m < M; ++m)
        for (int i = 0; i <= q; ++i)
        {
            cs (m, i) = std::cos (i * omega[m]);
            sn (m, i) = std::sin (i * omega[m]);
        }

    // R(i) = (1/M) sum_m P_m cos (i w_m).
    std::array<double, maxLpcOrder + 1> R {};
    for (int i = 0; i <= q; ++i)
    {
        double acc = 0.0;
        for (int m = 0; m < M; ++m)
            acc += P[m] * cs (m, i);
        R[(size_t) i] = acc / M;
    }
    // Starting point: the LPC (Levinson-Durbin) solution of the harmonic samples, order q.
    std::array<double, maxLpcOrder + 1> a {}, previous {};
    a[0] = 1.0;
    {
        double e = R[0] * (1.0 + 1.0e-7);
        for (int m = 1; m <= q; ++m)
        {
            double acc = R[(size_t) m];
            for (int i = 1; i < m; ++i) acc += a[(size_t) i] * R[(size_t) (m - i)];
            const double km = std::clamp (-acc / e, -(double) maxReflection, (double) maxReflection);
            previous = a;
            for (int i = 1; i < m; ++i) a[(size_t) i] = previous[(size_t) i] + km * previous[(size_t) (m - i)];
            a[(size_t) m] = km;
            e *= 1.0 - km * km;
            k[m - 1] = (float) km;
        }
    }

    const auto response = [&] (const std::array<double, maxLpcOrder + 1>& c, int m, double& re, double& im)
    {
        re = 0.0;
        im = 0.0;
        for (int i = 0; i <= q; ++i)
        {
            re += c[(size_t) i] * cs (m, i);
            im -= c[(size_t) i] * sn (m, i);
        }
    };
    for (int iteration = 0; iteration < 16; ++iteration)
    {
        // h(-i) = (1/M) sum_m Re (e^{-j i w_m} / A(w_m)).
        double h[maxLpcOrder + 1] {};
        for (int m = 0; m < M; ++m)
        {
            double re, im;
            response (a, m, re, im);
            const double mag = re * re + im * im;
            const double ir = re / mag, ii = -im / mag; // 1 / A
            for (int i = 0; i <= q; ++i)
                h[i] += ir * cs (m, i) + ii * sn (m, i);
        }
        // Solve the Toeplitz system R b = h (Gaussian elimination with partial pivoting; q <= 32).
        double A[maxLpcOrder + 1][maxLpcOrder + 2];
        for (int i = 0; i <= q; ++i)
        {
            for (int j = 0; j <= q; ++j)
                A[i][j] = R[(size_t) std::abs (i - j)];
            A[i][q + 1] = h[i] / M;
        }
        bool singular = false;
        for (int c = 0; c <= q && ! singular; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r <= q; ++r)
                if (std::abs (A[r][c]) > std::abs (A[pivot][c])) pivot = r;
            if (std::abs (A[pivot][c]) < 1.0e-300) { singular = true; break; }
            if (pivot != c)
                for (int j = 0; j <= q + 1; ++j) std::swap (A[c][j], A[pivot][j]);
            for (int r = c + 1; r <= q; ++r)
            {
                const double f = A[r][c] / A[c][c];
                for (int j = c; j <= q + 1; ++j) A[r][j] -= f * A[c][j];
            }
        }
        if (singular)
            break;
        double b[maxLpcOrder + 1];
        for (int i = q; i >= 0; --i)
        {
            double acc = A[i][q + 1];
            for (int j = i + 1; j <= q; ++j) acc -= A[i][j] * b[j];
            b[i] = acc / A[i][i];
        }
        if (! (std::abs (b[0]) > 1.0e-300))
            break;
        std::array<double, maxLpcOrder + 1> next {};
        for (int i = 0; i <= q; ++i)
            next[(size_t) i] = 0.5 * a[(size_t) i] + 0.5 * b[i] / b[0];
        float kNext[maxLpcOrder] {};
        if (! stepDown (next, q, kNext))
            break; // keep the last stable model
        a = next;
        std::copy (kNext, kNext + q, k);
    }

    // The gain that minimises E for this A: g^2 = (1/M) sum_m P_m |A(w_m)|^2.
    double g2 = 0.0;
    for (int m = 0; m < M; ++m)
    {
        double re, im;
        response (a, m, re, im);
        g2 += P[m] * (re * re + im * im);
    }
    error = (float) (g2 / M);
}

void LpcAnalyzer::analyseTrueEnvelope (const float* x, int n, double f0, float* k, float& error)
{
    const int count = powerSpectrum (x, n);
    double peak = 0.0;
    for (int b = 0; b < count; ++b) peak = std::max (peak, power[(size_t) b]);
    if (! (peak > 0.0))
    {
        std::fill (k, k + p, 0.0f);
        error = 0.0f;
        return;
    }
    // Log amplitude, floored 80 dB under the peak (the empty bins between harmonics would
    // otherwise pull the first smoothing pass down to the numerical floor).
    for (int b = 0; b < count; ++b)
        logAmplitude[(size_t) b] = 0.5 * std::log (std::max (power[(size_t) b], 1.0e-8 * peak));
    const int order = std::clamp ((int) std::floor (fs / (2.0 * f0)), 4, fftSize / 4);
    const int half = fftSize / 2;
    std::vector<double>& A = smooth; // A_i, updated in place; `power` holds V_i after each pass
    std::copy (logAmplitude.begin(), logAmplitude.end(), A.begin());
    const double tolerance = 2.0 / 8.685889638; // 2 dB in natural-log amplitude
    const int topBin = std::min (half, (int) (std::min (0.5 * fs, 8000.0) * fftSize / fs));
    for (int iteration = 0; iteration < 40; ++iteration)
    {
        // Real cepstrum of the even sequence A: c[q] = (1/N) sum_k A[k] cos (2 pi k q / N).
        for (int i = 0; i < fftSize; ++i)
            cepstrum[(size_t) i] = (float) A[(size_t) (i <= half ? i : fftSize - i)];
        std::fill (cepstrum.begin() + fftSize, cepstrum.end(), 0.0f);
        fft->performRealOnlyForwardTransform (cepstrum.data());
        // Lifter (Hamming, order `order`), then back to a log spectrum.
        for (int i = 0; i < fftSize; ++i)
        {
            const int q = i <= half ? i : fftSize - i;
            const double w = q <= order ? 0.54 + 0.46 * std::cos (pi * q / order) : 0.0;
            spectrum[(size_t) i] = (float) (w * cepstrum[(size_t) (2 * (i <= half ? i : fftSize - i))] / fftSize);
        }
        std::fill (spectrum.begin() + fftSize, spectrum.end(), 0.0f);
        fft->performRealOnlyForwardTransform (spectrum.data());
        double worst = 0.0;
        for (int b = 0; b <= half; ++b)
        {
            const double v = spectrum[(size_t) (2 * b)];
            if (b <= topBin) worst = std::max (worst, logAmplitude[(size_t) b] - v);
            A[(size_t) b] = std::max (A[(size_t) b], v);
            power[(size_t) b] = v; // keep V for the fit below
        }
        if (worst < tolerance)
            break;
    }
    // Power envelope exp (2 V) -> autocorrelation r[m] = (1/N) [P_0 + (-1)^m P_half + 2 sum P_b cos (2 pi b m / N)].
    std::array<double, maxLpcOrder + 1> r {};
    for (int m = 0; m <= p; ++m)
    {
        double acc = std::exp (2.0 * power[0]) + ((m & 1) ? -1.0 : 1.0) * std::exp (2.0 * power[(size_t) half]);
        for (int b = 1; b < half; ++b)
            acc += 2.0 * std::exp (2.0 * power[(size_t) b]) * std::cos (2.0 * pi * b * m / fftSize);
        r[(size_t) m] = acc / fftSize;
    }
    solve (r, k, error, false);
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
