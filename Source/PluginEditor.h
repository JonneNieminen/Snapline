#pragma once
#include <JuceHeader.h>
#include "DesignTokens.h"
#include "PitchGraph.h"
#include "PluginProcessor.h"

class SnaplineLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void setPalette (const snapline::theme::Palette&);
    const snapline::theme::Palette& palette() const { return colours; }

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawCornerResizer (juce::Graphics&, int width, int height, bool isMouseOver, bool isMouseDragging) override;

private:
    snapline::theme::Palette colours = snapline::theme::darkPalette();
};

/** A flat segmented control (as in Prism), e.g. Live | Studio. */
class Segmented final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Segmented (juce::StringArray options);
    void setSelected (int index, bool notify);
    int getSelected() const { return selected; }
    std::function<void (int)> onChange;
    void setPalette (const snapline::theme::Palette* p) { colours = p; repaint(); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    juce::StringArray labels;
    int selected = 0;
    const snapline::theme::Palette* colours = nullptr;
};

/** One labelled horizontal slider row: name, track, value. */
class ParameterRow final : public juce::Component
{
public:
    ParameterRow (juce::AudioProcessorValueTreeState&, const juce::String& parameterId, const juce::String& name,
                  const juce::String& tooltip);
    void setPalette (const snapline::theme::Palette* p) { colours = p; repaint(); }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider slider;
    juce::String label;
    const snapline::theme::Palette* colours = nullptr;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

/** One octave of keys showing target notes, held MIDI notes and the current note. Click to edit. */
class NoteStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    NoteStrip();
    void setPalette (const snapline::theme::Palette* p) { colours = p; repaint(); }
    void setState (snapline::NoteMask active, snapline::NoteMask held, int currentPitchClass);
    std::function<void (int)> onToggle;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> keyBounds (int pitchClass) const;
    int keyAt (juce::Point<float>) const;
    snapline::NoteMask active = snapline::allNotes, held = 0;
    int current = -1;
    const snapline::theme::Palette* colours = nullptr;
};

class SnaplineAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit SnaplineAudioProcessorEditor (SnaplineAudioProcessor&);
    ~SnaplineAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyTheme();
    void configureCombo (juce::ComboBox&, const juce::String& title, const juce::String& tooltip);
    void rebuildPresetMenu();
    void promptSavePreset();
    float uiScale() const { return (float) getWidth() / 1080.0f; }
    juce::Rectangle<int> toEditor (juce::Rectangle<int> baseArea) const;
    void paintCard (juce::Graphics&, juce::Rectangle<int>, const juce::String& title);
    void paintWordmark (juce::Graphics&, juce::Point<float>);
    void paintHeader (juce::Graphics&);
    juce::String scaleSummary() const;

    SnaplineAudioProcessor& audioProcessor;
    SnaplineLookAndFeel lookAndFeel;
    snapline::theme::Palette colours;
    juce::TooltipWindow tooltipWindow { this, 600 };

    juce::ComboBox preset, key, scale, inputType, midiMode, formantMethod;
    NoteStrip noteStrip;
    PitchGraph graph;
    Segmented modeSwitch { { "Live", "Studio" } };
    Segmented themeSwitch { { "Dark", "Light" } };
    juce::ToggleButton preserveFormants { "Preserve formants" };
    juce::ToggleButton bypass { "Bypass" };
    juce::Component holder;                // all controls live here at 1080 x 760 and are scaled as one
    juce::StringArray userPresetNames;

    ParameterRow retune, humanize, tolerance, amount;
    ParameterRow vibratoDepth, vibratoRate, vibratoOnset, naturalVibrato;
    ParameterRow formantShift, throat, transpose;
    ParameterRow mix, output, reference;

    std::unique_ptr<juce::ParameterAttachment> modeAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<ComboAttachment> keyAttachment, scaleAttachment, inputTypeAttachment, midiModeAttachment, formantMethodAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> preserveAttachment, bypassAttachment;

    juce::Rectangle<int> sidebar, header, cards[4];
    juce::String headerTitle, headerSubtitle;
    juce::StringArray chips;
    float displayedDelayMs = 0.0f;
    int currentPitchClass = -1;
    std::vector<snapline::DisplayFrame> frameBuffer = std::vector<snapline::DisplayFrame> (1024);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnaplineAudioProcessorEditor)
};
