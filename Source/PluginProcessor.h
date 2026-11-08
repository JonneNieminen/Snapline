#pragma once
#include <JuceHeader.h>
#include "SnaplineDsp.h"
#include "Presets.h"

class SnaplineAudioProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorValueTreeState::Listener,
                                     private juce::AsyncUpdater
{
public:
    SnaplineAudioProcessor();
    ~SnaplineAudioProcessor() override;
    static constexpr int numPresets = (int) snapline::factoryPresets.size();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return parameters.getParameter ("bypass"); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return numPresets; }
    int getCurrentProgram() override { return currentPreset.load(); }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** Target notes after key, scale, custom notes and MIDI are applied. */
    snapline::NoteMask getActiveNotes() const { return activeNotes.load (std::memory_order_relaxed); }
    snapline::NoteMask getHeldMidiNotes() const { return heldMask.load (std::memory_order_relaxed); }
    snapline::NoteMask scaleNotes() const;

    /**
        User presets: correction-style settings (the same parameters a factory preset sets) as XML
        files in the user's application-data folder. Message thread only; never touched by the
        audio thread.
    */
    juce::File getUserPresetDirectory() const;
    void setUserPresetDirectory (const juce::File&);          // tests and portable setups
    juce::StringArray listUserPresets() const;                 // names, sorted
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::String& name);
    juce::String getUserPresetName() const;                    // empty when a factory preset is current

    /** Copies the current scale into the note toggles, flips one note and switches to Custom. */
    void toggleNote (int pitchClass);

    int popDisplayFrames (snapline::DisplayFrame* dest, int maxFrames) { return dsp.popDisplayFrames (dest, maxFrames); }
    double getDisplayFrameRate() const { return dsp.getFrameRate(); }
    double getCurrentSampleRate() const { return preparedRate.load(); }
    /** The delay you actually hear, in ms (Live mode adapts it to the voice). */
    float getEffectiveDelayMs() const
    {
        const double rate = preparedRate.load();
        return rate > 0.0 ? (float) (dsp.getEffectiveDelaySamples() * 1000.0 / rate) : 0.0f;
    }

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<bool> presetModified { false };
    /** "dark" or "light"; editor-only, saved with the session. */
    juce::String theme { "dark" };
    /** Editor size relative to 1080 x 760 (0.75-1.5); editor-only, saved with the session. */
    float editorScale = 1.0f;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    snapline::SnaplineDsp dsp;
    std::atomic<int> currentPreset { 0 }; // Natural: a new instance starts organic, not tight
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<snapline::NoteMask> activeNotes { snapline::allNotes };
    std::atomic<snapline::NoteMask> heldMask { 0 };
    std::array<bool, 128> held {};
    int lastHeldNote = -1;
    bool applyingPreset = false;
    mutable juce::CriticalSection presetLock;
    juce::String userPresetName;
    juce::File userPresetDirectoryOverride;
    juce::File findUserPresetFile (const juce::String& name) const;

    /** Raw parameter values, looked up once so the audio thread never searches by ID. */
    struct RawParameters
    {
        std::atomic<float>* mode = nullptr;
        std::atomic<float>* inputType = nullptr;
        std::atomic<float>* key = nullptr;
        std::atomic<float>* scale = nullptr;
        std::array<std::atomic<float>*, 12> notes {};
        std::atomic<float>* midiMode = nullptr;
        std::atomic<float>* retune = nullptr;
        std::atomic<float>* humanize = nullptr;
        std::atomic<float>* tolerance = nullptr;
        std::atomic<float>* amount = nullptr;
        std::atomic<float>* transpose = nullptr;
        std::atomic<float>* reference = nullptr;
        std::atomic<float>* vibratoDepth = nullptr;
        std::atomic<float>* vibratoRate = nullptr;
        std::atomic<float>* vibratoOnset = nullptr;
        std::atomic<float>* naturalVibrato = nullptr;
        std::atomic<float>* preserveFormants = nullptr;
        std::atomic<float>* formantShift = nullptr;
        std::atomic<float>* throat = nullptr;
        std::atomic<float>* mix = nullptr;
        std::atomic<float>* output = nullptr;
        std::atomic<float>* bypass = nullptr;
    } raw;

    snapline::SnaplineParams readParameters();
    void updateHeldNotes (const juce::MidiBuffer&);
    void parameterChanged (const juce::String&, float) override;
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnaplineAudioProcessor)
};
