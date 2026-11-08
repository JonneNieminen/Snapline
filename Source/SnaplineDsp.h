#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <memory>
#include "Scales.h"

namespace snapline
{
enum class Mode { live = 0, studio = 1 };

enum class InputType { soprano = 0, altoTenor = 1, lowMale = 2, instrument = 3 };

/** Everything the engine needs, in user-facing units. */
struct SnaplineParams
{
    Mode mode = Mode::studio;
    InputType inputType = InputType::altoTenor;
    NoteMask allowed = allNotes;    // target pitch classes; 0 = no correction
    int forceNote = -1;             // MIDI note every voiced frame is pulled to, or -1
    float retuneMs = 20.0f;         // 0 = instant (hard tune)
    float humanize = 0.0f;          // %: slows correction on sustained notes
    float tolerance = 0.0f;         // %: only corrects notes the singer is already near
    float amount = 100.0f;          // %: how much of the correction is applied
    float transpose = 0.0f;         // semitones
    float referenceHz = 440.0f;     // A4
    float vibratoDepth = 0.0f;      // cents (peak)
    float vibratoRate = 5.5f;       // Hz
    float vibratoOnsetMs = 250.0f;  // delay after a note starts
    float naturalVibrato = 0.0f;    // %: -100 removes the singer's own vibrato, 0 keeps it, +100 doubles it
    bool preserveFormants = true;
    float formantShift = 0.0f;      // semitones
    float throat = 100.0f;          // % of natural throat length
    float mix = 100.0f;             // %
    float outputDb = 0.0f;
};

/** One analysis frame for the interface. */
struct DisplayFrame
{
    float inputHz = 0.0f;
    float outputHz = 0.0f;
    float correctionCents = 0.0f; // applied shift without transpose and vibrato
    int targetNote = -1;          // MIDI note, -1 when unvoiced
};

/**
    Real-time pitch correction.

    Mono analysis -> YIN pitch track -> note decision and retune smoothing ->
    pitch-synchronous overlap-add (TD-PSOLA) resynthesis. Grains are two pitch
    periods long and centred on glottal-pulse-like peaks, so the vocal tract
    response (formants) stays put while the pitch moves. Formant shift and throat
    length resample the grains independently of pitch.

    Studio mode delays the output by a fixed 100 ms: grain sources are always
    available, the pitch decision looks ahead and the delay is reported exactly.
    Live mode starts at 8 ms and grows only as far as the voice requires
    (about two to three periods).

    All memory is allocated in prepare(); process() is real-time safe. The display
    FIFO is single producer (audio thread) / single consumer (UI thread).
*/
class SnaplineDsp
{
public:
    SnaplineDsp();
    ~SnaplineDsp();

    void prepare (double sampleRate, int maxBlockSize, Mode initialMode);
    void reset();

    /** Fixed delay for a mode at the prepared sample rate. */
    int getLatencySamples (Mode mode) const;
    Mode getMode() const;

    /** inR / outR may be null for a mono bus. Input and output may alias. */
    void process (const float* inL, const float* inR, float* outL, float* outR,
                  int numSamples, const SnaplineParams& params);

    /** UI thread: copies up to maxFrames pending frames, oldest first. */
    int popDisplayFrames (DisplayFrame* dest, int maxFrames);

    /** Frames per second written to the display FIFO. */
    double getFrameRate() const;

    /** Current input-to-output delay in samples (Live mode adapts it to the voice). Any thread. */
    float getEffectiveDelaySamples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Pitch helpers shared with the interface. */
inline float hzToMidi (float hz, float referenceHz = 440.0f)
{
    return 69.0f + 12.0f * std::log2 (hz / referenceHz);
}
}
