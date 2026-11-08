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
    };

    /** maxWindow: the largest integration window W that will be used (samples). */
    void prepare (double sampleRate, int maxWindow);

    /**
        frame must hold 2 * window contiguous samples (oldest first). Lags from
        minLag to window are searched. A frame quieter than minRms returns (with its rms set)
        before any FFT is computed. The FFT is the smallest power of two that fits 2 * window,
        so each input type pays only for its own range.
    */
    Result detect (const float* frame, int window, int minLag, float threshold, float minRms = 0.0f);

private:
    double fs = 48000.0;
    int maxOrder = 0;
    std::vector<std::unique_ptr<juce::dsp::FFT>> ffts; // indexed by order; only those in use are created
    std::vector<float> a, b;
    std::vector<double> prefix;
    std::vector<float> diff;
};
}
