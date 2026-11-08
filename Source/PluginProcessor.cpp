#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr auto modeId = "mode";
constexpr auto inputTypeId = "inputType";
constexpr auto keyId = "key";
constexpr auto scaleId = "scale";
constexpr auto midiModeId = "midiMode";
constexpr auto retuneId = "retune";
constexpr auto humanizeId = "humanize";
constexpr auto toleranceId = "tolerance";
constexpr auto amountId = "amount";
constexpr auto transposeId = "transpose";
constexpr auto referenceId = "reference";
constexpr auto vibDepthId = "vibratoDepth";
constexpr auto vibRateId = "vibratoRate";
constexpr auto vibOnsetId = "vibratoOnset";
constexpr auto naturalVibId = "naturalVibrato";
constexpr auto preserveId = "preserveFormants";
constexpr auto formantId = "formantShift";
constexpr auto throatId = "throat";
constexpr auto mixId = "mix";
constexpr auto outputId = "output";
constexpr auto bypassId = "bypass";

juce::String noteId (int pitchClass) { return "note" + juce::String (pitchClass); }

/** Parameters a factory preset sets; moving one marks the preset as modified. */
const juce::StringArray presetParameterIds { retuneId, humanizeId, toleranceId, amountId, vibDepthId, vibRateId,
                                             vibOnsetId, naturalVibId, preserveId, formantId, throatId };

juce::String formatValue (float value, const char* unit, int decimals)
{
    // Whole numbers go through an integer: String (float, 0) prints values like 1.5e-8 in exponent form.
    if (decimals <= 0)
        return juce::String (juce::roundToInt (value)) + unit;
    const auto text = juce::String (value, decimals);
    const auto clean = text.containsOnly ("-0.") ? text.trimCharactersAtStart ("-") : text;
    return clean + unit;
}

juce::AudioParameterFloatAttributes withUnit (const char* unit, int decimals)
{
    return juce::AudioParameterFloatAttributes()
        .withLabel (juce::String (unit).trim())
        .withStringFromValueFunction ([unit, decimals] (float v, int) { return formatValue (v, unit, decimals); });
}

float read (const std::atomic<float>* value)
{
    return value->load (std::memory_order_relaxed);
}

void setParameterValue (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value)
{
    if (auto* parameter = state.getParameter (id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        parameter->endChangeGesture();
    }
}
}

SnaplineAudioProcessor::SnaplineAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "SNAPLINE_STATE", createParameterLayout())
{
    const auto get = [this] (const juce::String& id) { return parameters.getRawParameterValue (id); };
    raw.mode = get (modeId);
    raw.inputType = get (inputTypeId);
    raw.key = get (keyId);
    raw.scale = get (scaleId);
    for (int n = 0; n < 12; ++n)
        raw.notes[(size_t) n] = get (noteId (n));
    raw.midiMode = get (midiModeId);
    raw.retune = get (retuneId);
    raw.humanize = get (humanizeId);
    raw.tolerance = get (toleranceId);
    raw.amount = get (amountId);
    raw.transpose = get (transposeId);
    raw.reference = get (referenceId);
    raw.vibratoDepth = get (vibDepthId);
    raw.vibratoRate = get (vibRateId);
    raw.vibratoOnset = get (vibOnsetId);
    raw.naturalVibrato = get (naturalVibId);
    raw.preserveFormants = get (preserveId);
    raw.formantShift = get (formantId);
    raw.throat = get (throatId);
    raw.mix = get (mixId);
    raw.output = get (outputId);
    raw.bypass = get (bypassId);

    for (const auto& id : presetParameterIds)
        parameters.addParameterListener (id, this);
}

SnaplineAudioProcessor::~SnaplineAudioProcessor()
{
    cancelPendingUpdate();
    for (const auto& id : presetParameterIds)
        parameters.removeParameterListener (id, this);
}

void SnaplineAudioProcessor::parameterChanged (const juce::String&, float)
{
    if (! applyingPreset)
        presetModified.store (true);
}

juce::AudioProcessorValueTreeState::ParameterLayout SnaplineAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Bool = juce::AudioParameterBool;
    using Range = juce::NormalisableRange<float>;
    juce::AudioProcessorValueTreeState::ParameterLayout p;

    p.add (std::make_unique<Choice> (juce::ParameterID { modeId, 1 }, "Mode", juce::StringArray { "Live", "Studio" }, 1,
                                     juce::AudioParameterChoiceAttributes().withAutomatable (false)));
    p.add (std::make_unique<Choice> (juce::ParameterID { inputTypeId, 1 }, "Input Type",
                                     juce::StringArray { "Soprano", "Alto/Tenor", "Low Male", "Instrument" }, 1));

    juce::StringArray keys, scaleNames;
    for (auto* name : snapline::noteNames) keys.add (name);
    for (const auto& s : snapline::scales) scaleNames.add (s.name);
    p.add (std::make_unique<Choice> (juce::ParameterID { keyId, 1 }, "Key", keys, 0));
    p.add (std::make_unique<Choice> (juce::ParameterID { scaleId, 1 }, "Scale", scaleNames, 0));
    for (int n = 0; n < 12; ++n)
        p.add (std::make_unique<Bool> (juce::ParameterID { noteId (n), 1 }, juce::String ("Note ") + snapline::noteNames[(size_t) n], true));
    p.add (std::make_unique<Choice> (juce::ParameterID { midiModeId, 1 }, "MIDI Notes",
                                     juce::StringArray { "Off", "Snap to Held Notes", "Force to Held Note" }, 0));

    p.add (std::make_unique<Float> (juce::ParameterID { retuneId, 1 }, "Retune Speed", Range (0.0f, 400.0f, 0.1f, 0.4f), 85.0f, withUnit (" ms", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { humanizeId, 1 }, "Humanize", Range (0.0f, 100.0f, 0.1f), 45.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { toleranceId, 1 }, "Tolerance", Range (0.0f, 100.0f, 0.1f), 35.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { amountId, 1 }, "Amount", Range (0.0f, 100.0f, 0.1f), 100.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { transposeId, 1 }, "Transpose", Range (-12.0f, 12.0f, 1.0f), 0.0f, withUnit (" st", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { referenceId, 1 }, "A4 Reference", Range (415.0f, 466.0f, 0.1f), 440.0f, withUnit (" Hz", 1)));
    p.add (std::make_unique<Float> (juce::ParameterID { vibDepthId, 1 }, "Vibrato Depth", Range (0.0f, 100.0f, 0.1f), 0.0f, withUnit (" ct", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { vibRateId, 1 }, "Vibrato Rate", Range (2.0f, 9.0f, 0.01f), 5.5f, withUnit (" Hz", 1)));
    p.add (std::make_unique<Float> (juce::ParameterID { vibOnsetId, 1 }, "Vibrato Onset", Range (0.0f, 1000.0f, 1.0f), 250.0f, withUnit (" ms", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { naturalVibId, 1 }, "Natural Vibrato", Range (-100.0f, 100.0f, 0.1f), 0.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Bool> (juce::ParameterID { preserveId, 1 }, "Preserve Formants", true));
    p.add (std::make_unique<Float> (juce::ParameterID { formantId, 1 }, "Formant Shift", Range (-6.0f, 6.0f, 0.1f), 0.0f, withUnit (" st", 1)));
    p.add (std::make_unique<Float> (juce::ParameterID { throatId, 1 }, "Throat Length", Range (60.0f, 160.0f, 0.1f), 100.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { mixId, 1 }, "Mix", Range (0.0f, 100.0f, 0.1f), 100.0f, withUnit ("%", 0)));
    p.add (std::make_unique<Float> (juce::ParameterID { outputId, 1 }, "Output", Range (-24.0f, 12.0f, 0.01f), 0.0f, withUnit (" dB", 1)));
    p.add (std::make_unique<Bool> (juce::ParameterID { bypassId, 1 }, "Bypass", false));
    return p;
}

bool SnaplineAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return input == output && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void SnaplineAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto mode = (snapline::Mode) juce::jlimit (0, 1, juce::roundToInt (read (raw.mode)));
    dsp.prepare (sampleRate, samplesPerBlock, mode);
    preparedRate.store (sampleRate);
    held.fill (false);
    heldMask.store (0);
    lastHeldNote = -1;
    setLatencySamples (dsp.getLatencySamples (mode));
}

snapline::NoteMask SnaplineAudioProcessor::scaleNotes() const
{
    const int scale = juce::jlimit (0, (int) snapline::scales.size() - 1, juce::roundToInt (read (raw.scale)));
    if (scale == snapline::customScaleIndex)
    {
        snapline::NoteMask mask = 0;
        for (int n = 0; n < 12; ++n)
            if (read (raw.notes[(size_t) n]) >= 0.5f)
                mask = (snapline::NoteMask) (mask | (1u << n));
        return mask;
    }
    return snapline::maskForKey (snapline::scales[(size_t) scale].intervals, juce::jlimit (0, 11, juce::roundToInt (read (raw.key))));
}

void SnaplineAudioProcessor::updateHeldNotes (const juce::MidiBuffer& midi)
{
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())
        {
            held[(size_t) message.getNoteNumber()] = true;
            lastHeldNote = message.getNoteNumber();
        }
        else if (message.isNoteOff())
        {
            held[(size_t) message.getNoteNumber()] = false;
            if (lastHeldNote == message.getNoteNumber())
            {
                lastHeldNote = -1;
                for (int n = 127; n >= 0; --n)
                    if (held[(size_t) n]) { lastHeldNote = n; break; }
            }
        }
        else if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            held.fill (false);
            lastHeldNote = -1;
        }
    }
    snapline::NoteMask mask = 0;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n])
            mask = (snapline::NoteMask) (mask | (1u << (n % 12)));
    heldMask.store (mask, std::memory_order_relaxed);
}

snapline::SnaplineParams SnaplineAudioProcessor::readParameters()
{
    snapline::SnaplineParams p;
    p.mode = (snapline::Mode) juce::jlimit (0, 1, juce::roundToInt (read (raw.mode)));
    p.inputType = (snapline::InputType) juce::jlimit (0, 3, juce::roundToInt (read (raw.inputType)));

    auto notes = scaleNotes();
    const int midiMode = juce::roundToInt (read (raw.midiMode));
    const auto heldNotes = heldMask.load (std::memory_order_relaxed);
    if (midiMode == 1 && heldNotes != 0)
        notes = heldNotes;
    if (midiMode == 2 && lastHeldNote >= 0)
    {
        p.forceNote = lastHeldNote;
        notes = (snapline::NoteMask) (1u << (lastHeldNote % 12));
    }
    p.allowed = notes;
    activeNotes.store (notes, std::memory_order_relaxed);

    p.retuneMs = read (raw.retune);
    p.humanize = read (raw.humanize);
    p.tolerance = read (raw.tolerance);
    p.amount = read (raw.amount);
    p.transpose = read (raw.transpose);
    p.referenceHz = read (raw.reference);
    p.vibratoDepth = read (raw.vibratoDepth);
    p.vibratoRate = read (raw.vibratoRate);
    p.vibratoOnsetMs = read (raw.vibratoOnset);
    p.naturalVibrato = read (raw.naturalVibrato);
    p.preserveFormants = read (raw.preserveFormants) >= 0.5f;
    p.formantShift = read (raw.formantShift);
    p.throat = read (raw.throat);
    p.mix = read (raw.mix);
    p.outputDb = read (raw.output);
    p.bypass = read (raw.bypass) >= 0.5f;
    return p;
}

void SnaplineAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin (buffer.getNumChannels(), 2);
    const int samples = buffer.getNumSamples();
    updateHeldNotes (midi);
    midi.clear();
    if (channels == 0 || samples == 0)
        return;

    const auto params = readParameters();
    dsp.process (buffer.getReadPointer (0), channels > 1 ? buffer.getReadPointer (1) : nullptr,
                 buffer.getWritePointer (0), channels > 1 ? buffer.getWritePointer (1) : nullptr, samples, params);
    if (dsp.getLatencySamples (dsp.getMode()) != getLatencySamples())
        triggerAsyncUpdate();

    float peak = 0.0f;
    for (int ch = 0; ch < channels; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, samples));
    outputPeak.store (juce::jmax (peak, outputPeak.load (std::memory_order_relaxed) * 0.88f), std::memory_order_relaxed);
}

void SnaplineAudioProcessor::handleAsyncUpdate()
{
    setLatencySamples (dsp.getLatencySamples (dsp.getMode()));
}

void SnaplineAudioProcessor::toggleNote (int pitchClass)
{
    const auto current = scaleNotes();
    const auto next = (snapline::NoteMask) (current ^ (1u << juce::jlimit (0, 11, pitchClass)));
    for (int n = 0; n < 12; ++n)
        setParameterValue (parameters, noteId (n), (next & (1u << n)) != 0 ? 1.0f : 0.0f);
    setParameterValue (parameters, scaleId, (float) snapline::customScaleIndex);
}

void SnaplineAudioProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, numPresets - 1, index);
    const auto& v = snapline::factoryPresets[(size_t) index].values;
    applyingPreset = true;
    setParameterValue (parameters, retuneId, v.retuneMs);
    setParameterValue (parameters, humanizeId, v.humanize);
    setParameterValue (parameters, toleranceId, v.tolerance);
    setParameterValue (parameters, amountId, v.amount);
    setParameterValue (parameters, vibDepthId, v.vibratoDepth);
    setParameterValue (parameters, vibRateId, v.vibratoRate);
    setParameterValue (parameters, vibOnsetId, v.vibratoOnsetMs);
    setParameterValue (parameters, naturalVibId, v.naturalVibrato);
    setParameterValue (parameters, preserveId, v.preserveFormants ? 1.0f : 0.0f);
    setParameterValue (parameters, formantId, v.formantShift);
    setParameterValue (parameters, throatId, v.throat);
    applyingPreset = false;
    currentPreset.store (index);
    presetModified.store (false);
    const juce::ScopedLock lock (presetLock);
    userPresetName = {};
}

// ---------------------------------------------------------------------------- user presets
juce::File SnaplineAudioProcessor::getUserPresetDirectory() const
{
    if (userPresetDirectoryOverride != juce::File())
        return userPresetDirectoryOverride;
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Snapline").getChildFile ("Presets");
}

void SnaplineAudioProcessor::setUserPresetDirectory (const juce::File& directory) { userPresetDirectoryOverride = directory; }

juce::String SnaplineAudioProcessor::getUserPresetName() const
{
    const juce::ScopedLock lock (presetLock);
    return userPresetName;
}

namespace
{
constexpr auto presetTag = "SnaplinePreset";

std::unique_ptr<juce::XmlElement> readPresetFile (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    return xml != nullptr && xml->hasTagName (presetTag) ? std::move (xml) : nullptr;
}
}

juce::StringArray SnaplineAudioProcessor::listUserPresets() const
{
    juce::StringArray names;
    for (const auto& file : getUserPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.xml"))
        if (auto xml = readPresetFile (file))
            names.add (xml->getStringAttribute ("name", file.getFileNameWithoutExtension()));
    names.sort (true);
    return names;
}

juce::File SnaplineAudioProcessor::findUserPresetFile (const juce::String& name) const
{
    for (const auto& file : getUserPresetDirectory().findChildFiles (juce::File::findFiles, false, "*.xml"))
        if (auto xml = readPresetFile (file))
            if (xml->getStringAttribute ("name", file.getFileNameWithoutExtension()) == name)
                return file;
    return {};
}

bool SnaplineAudioProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = rawName.trim().substring (0, 64);
    const auto fileName = juce::File::createLegalFileName (name);
    if (name.isEmpty() || fileName.isEmpty())
        return false;
    auto directory = getUserPresetDirectory();
    if (! directory.createDirectory())
        return false;

    juce::XmlElement root (presetTag);
    root.setAttribute ("version", 1);
    root.setAttribute ("name", name);
    for (const auto& id : presetParameterIds)
        if (auto* parameter = parameters.getParameter (id))
        {
            auto* child = root.createNewChildElement ("Param");
            child->setAttribute ("id", id);
            child->setAttribute ("value", (double) parameter->convertFrom0to1 (parameter->getValue()));
        }

    auto file = findUserPresetFile (name);
    if (file == juce::File())
        file = directory.getChildFile (fileName + ".xml");
    if (! file.replaceWithText (root.toString()))
        return false;
    const juce::ScopedLock lock (presetLock);
    userPresetName = name;
    presetModified.store (false);
    return true;
}

bool SnaplineAudioProcessor::loadUserPreset (const juce::String& name)
{
    const auto file = findUserPresetFile (name);
    if (file == juce::File())
        return false;
    auto xml = readPresetFile (file);
    if (xml == nullptr)
        return false;
    applyingPreset = true;
    for (auto* child : xml->getChildWithTagNameIterator ("Param"))
    {
        const auto id = child->getStringAttribute ("id");
        if (presetParameterIds.contains (id)) // never key, scale, input type, mode, MIDI, mix or output
            setParameterValue (parameters, id, (float) child->getDoubleAttribute ("value"));
    }
    applyingPreset = false;
    presetModified.store (false);
    const juce::ScopedLock lock (presetLock);
    userPresetName = name;
    return true;
}

const juce::String SnaplineAudioProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, numPresets) ? juce::String (snapline::factoryPresets[(size_t) index].name) : juce::String();
}

void SnaplineAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty ("preset", currentPreset.load(), nullptr);
    state.setProperty ("presetModified", presetModified.load(), nullptr);
    state.setProperty ("theme", theme, nullptr);
    state.setProperty ("editorScale", editorScale, nullptr);
    state.setProperty ("userPreset", getUserPresetName(), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, dest);
}

void SnaplineAudioProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
    {
        const auto state = juce::ValueTree::fromXml (*xml);
        currentPreset.store (juce::jlimit (0, numPresets - 1, (int) state.getProperty ("preset", 2)));
        applyingPreset = true;
        parameters.replaceState (state);
        applyingPreset = false;
        presetModified.store ((bool) state.getProperty ("presetModified", ! state.hasProperty ("preset")));
        theme = state.getProperty ("theme", "dark").toString() == "light" ? "light" : "dark";
        editorScale = juce::jlimit (0.75f, 1.5f, (float) (double) state.getProperty ("editorScale", 1.0));
        const juce::ScopedLock lock (presetLock);
        userPresetName = state.getProperty ("userPreset", "").toString();
    }
}

juce::AudioProcessorEditor* SnaplineAudioProcessor::createEditor() { return new SnaplineAudioProcessorEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SnaplineAudioProcessor(); }
