#pragma once
#include <JuceHeader.h>
#include "DesignTokens.h"
#include "SnaplineDsp.h"

/**
    Scrolling pitch display: the detected input (faint) and the corrected output
    (vocal colour) over the last few seconds, drawn on semitone lanes with the
    target notes highlighted. The header shows the current note, how far the
    singer is from it and how much correction is being applied.
*/
class PitchGraph final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    PitchGraph();

    void setPalette (const snapline::theme::Palette* palette) { colours = palette; repaint(); }
    void setFrameRate (double framesPerSecond) { frameRate = juce::jmax (1.0, framesPerSecond); }
    void setNotes (snapline::NoteMask activeNotes, float referenceHz);
    void push (const snapline::DisplayFrame& frame);
    /** Eases the vertical range towards the singer; call from the UI timer. */
    void advance (float seconds);

    void paint (juce::Graphics&) override;

private:
    static constexpr int capacity = 4096;
    static constexpr float visibleSeconds = 5.0f;
    static constexpr float visibleSemitones = 15.0f;

    const snapline::theme::Palette* colours = nullptr;
    std::vector<snapline::DisplayFrame> history;
    int writeIndex = 0, stored = 0;
    double frameRate = 375.0;
    snapline::NoteMask notes = snapline::allNotes;
    float reference = 440.0f;
    float viewCentre = 57.0f;     // MIDI note at the vertical centre
    snapline::DisplayFrame latest;
    int framesSinceVoiced = 1 << 20;

    const snapline::DisplayFrame& at (int age) const; // 0 = newest
    void paintHeader (juce::Graphics&, juce::Rectangle<float>);
    void paintLanes (juce::Graphics&, juce::Rectangle<float> plot, juce::Rectangle<float> gutter);
    void paintTraces (juce::Graphics&, juce::Rectangle<float> plot);
    float midiToY (float midi, juce::Rectangle<float> plot) const;
};
