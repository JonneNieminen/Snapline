#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace snapline
{
constexpr int maxLpcOrder = 32;

/**
    Linear-predictive (all-pole) model of the vocal tract, used by the two LPC formant methods.

    Source-filter view of the voice: s = e * h, a glottal excitation e (pulses, roughly flat
    spectrum) shaped by the vocal tract h, whose resonances are the formants. LPC fits an
    all-pole model
        H(z) = g / A(z),   A(z) = 1 + a_1 z^-1 + ... + a_p z^-p,
    by minimising the energy of the prediction error e[n] = A(z) s[n]. The optimal a_i solve the
    Toeplitz normal equations R a = -r built from the frame's autocorrelation, which
    Levinson-Durbin solves in O(p^2). It also yields the reflection coefficients k_m (|k_m| < 1
    exactly when A is minimum phase, i.e. 1/A is stable) and the error power E = g^2.

    Everything is kept in reflection-coefficient (lattice) form. Coefficients move every analysis
    hop (2.7 ms), and interpolating direct-form a_i can create unstable poles mid-way;
    interpolating k_m cannot (any blend of values in (-1, 1) stays in (-1, 1)).

    Analysis, warping and filtering are allocation-free after prepare().
*/
class LpcAnalyzer
{
public:
    /** maxFrame: the longest analysis frame (samples) that will be passed to analyse(). */
    void prepare (double sampleRate, int maxFrame);

    /** Model order for the prepared rate: 16 up to 48 kHz (about one pole pair per kHz of
        a 5 kHz formant region, plus tilt), scaled with the rate above it, at most 32. */
    int order() const { return p; }

    /**
        Fits the model to n samples (oldest first): Hann window, pre-emphasis (1 - 0.97 z^-1, so
        the -12 dB/oct glottal tilt does not use up poles), autocorrelation r[0..p], a 60 Hz
        Gaussian lag window (smooths the envelope so it follows formants, not single harmonics)
        and a -40 dB white-noise floor (keeps R well conditioned). Silent frames give k = 0.
    */
    void analyse (const float* x, int n, float* k, float& error);

    /**
        The same envelope moved in frequency by phi (phi > 1: formants up, shorter vocal tract).
        P(w) = E / |A(e^jw)|^2 is sampled on a 256-bin grid, read back at w / phi (the top of the
        band repeats its edge value when phi < 1), turned back into an autocorrelation by an
        inverse cosine transform, and refitted with Levinson-Durbin. gain = sqrt(E' / E), the
        level that makes  g' / A'  over  g / A  a pure envelope ratio.
    */
    void warp (const float* k, float error, double phi, float* kOut, float& gain);

private:
    static constexpr int bins = 256;
    double fs = 48000.0;
    int p = 16;
    std::vector<float> windowed;
    std::vector<double> lagWindow;
    std::vector<double> cosTable, sinTable; // [(order + 1) x (bins + 1)]: cos / sin (pi q i / bins)
    std::array<double, bins + 1> envelope {}, warped {};

    void solve (std::array<double, maxLpcOrder + 1>& r, float* k, float& error, bool condition) const;
};

/** FIR lattice: e = A(z) x (whitening / inverse filter). One instance per channel. */
struct LatticeInverse
{
    std::array<float, maxLpcOrder + 1> b {};
    void reset() { b.fill (0.0f); }

    float process (float x, const float* k, int order)
    {
        // Stage m:  f_m[n] = f_{m-1}[n] + k_m b_{m-1}[n-1],   b_m[n] = b_{m-1}[n-1] + k_m f_{m-1}[n].
        // b[m] holds b_m[n-1]; f_0 = b_0 = x.
        float f = x, bPrevious = x;
        for (int m = 0; m < order; ++m)
        {
            const float delayed = b[(size_t) m];
            const float fNext = f + k[m] * delayed;
            const float bNext = delayed + k[m] * f;
            b[(size_t) m] = bPrevious;
            bPrevious = bNext;
            f = fNext;
        }
        return f;
    }
};

/** IIR lattice: y = e / A(z) (all-pole synthesis). Stable whenever every |k| < 1. */
struct LatticeSynthesis
{
    std::array<float, maxLpcOrder + 1> b {};
    void reset() { b.fill (0.0f); }

    float process (float e, const float* k, int order)
    {
        // Runs the inverse lattice backwards: f_{m-1} = f_m - k_m b_{m-1}[n-1], and the backward
        // path b_m[n] = b_{m-1}[n-1] + k_m f_{m-1}[n] is rebuilt on the way down.
        float f = e;
        for (int m = order - 1; m >= 0; --m)
        {
            f -= k[m] * b[(size_t) m];
            b[(size_t) m + 1] = b[(size_t) m] + k[m] * f;
        }
        b[0] = f;
        return f;
    }
};
}
