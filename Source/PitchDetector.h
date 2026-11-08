#pragma once
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <vector>

namespace snapline
{
/**
    YIN fundamental-frequency estimator (de Cheveigné & Kawahara, 2002).

    The difference function is computed through an FFT cross-correlation, so a
    frame costs three FFTs regardless of the lag range. Everything is allocated
    in prepare(); detect() is real-time safe.
*/
class PitchDetector
{
public:
    struct Result
    {
        float hz = 0.0f;           // 0 when no period was found
        float aperiodicity = 1.0f; // YIN d'(tau) at the chosen lag: 0 = perfectly periodic
        float rms = 0.0f;
        float voicedProbability = 0.0f; // pYIN: share of the threshold prior under which some dip is found
    };

    /** maxWindow: the largest integration window W that will be used (samples). */
    void prepare (double sampleRate, int maxWindow);

    /**
        frame must hold 2 * window contiguous samples (oldest first). Lags from
        minLag to window are searched. A frame quieter than minRms returns (with its rms set)
        before any FFT is computed. The FFT is the smallest power of two that fits 2 * window,
        so each input type pays only for its own range.

        previousHz (0 = none) is the pitch of the previous voiced frame. pYIN-style candidate
        scoring (see detect()) then prefers dips near it, which keeps a breathy note from
        hopping to a sub-harmonic for a frame.
    */
    Result detect (const float* frame, int window, int minLag, float threshold, float minRms = 0.0f,
                   float previousHz = 0.0f);

private:
    double fs = 48000.0;
    int maxOrder = 0;
    std::vector<std::unique_ptr<juce::dsp::FFT>> ffts; // indexed by order; only those in use are created
    std::vector<float> a, b;
    std::vector<double> prefix;
    std::vector<float> diff;

    // pYIN threshold prior: CDF of Beta(2, 18) (mean 0.1) on a 1/1024 grid.
    static constexpr int priorSteps = 1024;
    std::vector<float> priorCdf;
    float thresholdCdf (float s) const;
};
}
