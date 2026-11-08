// snapline-snapshot: renders the plugin interface to PNG without a DAW.
// Usage: snapline-snapshot <output-directory>
// Feeds a synthetic off-key vocal through the real processor so the pitch display
// has content, then captures the editor in the dark and light themes.
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cmath>

namespace
{
constexpr double pi = 3.14159265358979323846;

void feed (SnaplineAudioProcessor& processor, double fs, double seconds)
{
    struct Note { double start, length, midi, vibrato; };
    const Note melody[] { { 0.1, 0.6, 57.35, 0 }, { 0.8, 0.6, 59.72, 0 }, { 1.5, 1.1, 62.18, 30 },
                          { 2.7, 0.5, 63.6, 0 }, { 3.3, 0.9, 60.25, 25 } };
    const int block = 256;
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    double phase = 0.0, y1[3] {}, y2[3] {};
    const double formants[3][3] { { 700, 110, 1.0 }, { 1220, 120, 0.6 }, { 2600, 160, 0.3 } };
    for (int pos = 0; pos < (int) (seconds * fs); pos += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const double t = (pos + i) / fs;
            double s = 0.0;
            for (const auto& n : melody)
            {
                if (t < n.start || t >= n.start + n.length) continue;
                const double age = t - n.start;
                const double m = n.midi + n.vibrato / 100.0 * std::sin (2 * pi * 5.5 * age) * std::min (1.0, age / 0.2);
                phase += 440.0 * std::pow (2.0, (m - 69.0) / 12.0) / fs;
                phase -= std::floor (phase);
                for (int k = 1; k <= 30; ++k) s += std::sin (2 * pi * k * phase) / k;
                s *= std::min ({ 1.0, age / 0.03, (n.length - age) / 0.04 });
            }
            double out = 0.0;
            for (int f = 0; f < 3; ++f)
            {
                const double rad = std::exp (-pi * formants[f][1] / fs);
                const double a1 = -2 * rad * std::cos (2 * pi * formants[f][0] / fs), a2 = rad * rad;
                const double y = (1 - rad) * s - a1 * y1[f] - a2 * y2[f];
                y2[f] = y1[f];
                y1[f] = y;
                out += y * formants[f][2];
            }
            buffer.setSample (0, i, (float) (out * 0.25));
            buffer.setSample (1, i, (float) (out * 0.25));
        }
        processor.processBlock (buffer, midi);
    }
}
}

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? " ok " : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

float value (SnaplineAudioProcessor& p, const char* id) { return p.parameters.getRawParameterValue (id)->load(); }

void set (SnaplineAudioProcessor& p, const char* id, float v)
{
    auto* parameter = p.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (v));
}

void collect (juce::Component& c, std::vector<juce::Component*>& out)
{
    for (auto* child : c.getChildren())
    {
        out.push_back (child);
        collect (*child, out);
    }
}

std::unique_ptr<SnaplineAudioProcessor> makeProcessor()
{
    std::unique_ptr<SnaplineAudioProcessor> p (static_cast<SnaplineAudioProcessor*> (createPluginFilter()));
    p->setPlayConfigDetails (2, 2, 48000.0, 256);
    p->prepareToPlay (48000.0, 256);
    return p;
}

/** Plugin-level checks that need no DAW: bypass parameter, user presets, session state, editor scaling, accessibility. */
int runChecks()
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Plugin checks\n");

    auto processor = makeProcessor();
    check (processor->getBypassParameter() != nullptr && processor->getBypassParameter()->getName (32) == "Bypass",
           "host bypass parameter is exposed (getBypassParameter)");

    // User presets: only the correction style is stored, as XML, in a folder of our choosing.
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("snapline-preset-check");
    folder.deleteRecursively();
    processor->setUserPresetDirectory (folder);
    set (*processor, "retune", 123.0f);
    set (*processor, "humanize", 61.0f);
    set (*processor, "naturalVibrato", -35.0f);
    set (*processor, "mix", 37.0f);
    set (*processor, "key", 4.0f);
    check (processor->saveUserPreset ("My Style"), "save a user preset");
    check (processor->saveUserPreset ("../escape/Name?"), "a hostile name is made safe");
    check (processor->listUserPresets().contains ("My Style") && folder.findChildFiles (juce::File::findFiles, false, "*.xml").size() == 2
               && ! folder.getParentDirectory().getChildFile ("escape").exists(),
           "presets are listed and stay inside the preset folder");
    const auto xml = juce::XmlDocument::parse (folder.findChildFiles (juce::File::findFiles, false, "My*.xml").getFirst());
    check (xml != nullptr && xml->hasTagName ("SnaplinePreset") && xml->getNumChildElements() == 11, "the file is XML with the 11 style parameters");
    set (*processor, "retune", 5.0f);
    set (*processor, "humanize", 0.0f);
    set (*processor, "naturalVibrato", 80.0f);
    set (*processor, "mix", 50.0f);
    set (*processor, "key", 7.0f);
    processor->presetModified.store (true);
    check (processor->loadUserPreset ("My Style"), "load a user preset");
    check (std::abs (value (*processor, "retune") - 123.0f) < 0.2f && std::abs (value (*processor, "humanize") - 61.0f) < 0.2f
               && std::abs (value (*processor, "naturalVibrato") + 35.0f) < 0.2f,
           "style parameters come back");
    check (std::abs (value (*processor, "mix") - 50.0f) < 0.2f && std::abs (value (*processor, "key") - 7.0f) < 0.2f,
           "mix and key are left alone");
    check (! processor->presetModified.load() && processor->getUserPresetName() == "My Style", "loaded preset is current, not modified");
    set (*processor, "retune", 40.0f);
    check (processor->presetModified.load(), "moving a style control marks it modified");
    check (! processor->loadUserPreset ("Does not exist"), "a missing preset fails cleanly");

    // Session state: preset name and editor size survive save and restore.
    processor->loadUserPreset ("My Style");
    processor->editorScale = 1.25f;
    juce::MemoryBlock state;
    processor->getStateInformation (state);
    auto restored = makeProcessor();
    restored->setStateInformation (state.getData(), (int) state.getSize());
    check (restored->getUserPresetName() == "My Style" && std::abs (restored->editorScale - 1.25f) < 1.0e-4f
               && std::abs (value (*restored, "retune") - 123.0f) < 0.2f,
           "user preset name, editor size and parameters restore");
    check (value (*restored, "bypass") < 0.5f, "bypass defaults to off");

    // Editor: resizable 75-150 %, every control has a title and a tooltip.
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditor());
    editor->setVisible (true);
    check (editor->getWidth() == 1350 && editor->getHeight() == 950, "editor opens at the saved size (125 %)");
    check (editor->isResizable(), "editor is resizable");
    auto* limits = editor->getConstrainer();
    check (limits != nullptr && limits->getMinimumWidth() == 810 && limits->getMinimumHeight() == 570, "minimum size is 75 %");
    check (limits != nullptr && limits->getMaximumWidth() == 1620 && limits->getMaximumHeight() == 1140, "maximum size is 150 %");
    juce::Rectangle<int> dragged (0, 0, 5000, 5000);
    limits->checkBounds (dragged, editor->getBounds(), juce::Rectangle<int> (0, 0, 4000, 4000), false, false, false, false);
    check (dragged.getWidth() <= 1620 && dragged.getHeight() <= 1140 && std::abs ((double) dragged.getWidth() / dragged.getHeight() - 1080.0 / 760.0) < 0.01,
           "dragging past the limit stays within 150 % at the same proportions");
    editor->setSize (1620, 1140);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    check (std::abs (processor->editorScale - 1.5f) < 1.0e-3f, "the editor reports its size to the session");
    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    check (image.getWidth() == 1620, "renders at 150 %");

    std::vector<juce::Component*> all;
    collect (*editor, all);
    int controls = 0, untitled = 0, untipped = 0;
    for (auto* c : all)
    {
        auto* tip = dynamic_cast<juce::SettableTooltipClient*> (c);
        const bool isControl = dynamic_cast<juce::Slider*> (c) != nullptr || dynamic_cast<juce::ComboBox*> (c) != nullptr
                               || dynamic_cast<juce::Button*> (c) != nullptr || tip != nullptr;
        if (! isControl || c->getParentComponent() == nullptr || dynamic_cast<juce::ComboBox*> (c->getParentComponent()) != nullptr)
            continue;
        ++controls;
        if (c->getTitle().isEmpty()) { ++untitled; std::printf ("      no title: %s\n", typeid (*c).name()); }
        if (tip != nullptr && tip->getTooltip().isEmpty()) { ++untipped; std::printf ("      no tooltip: %s (%s)\n", typeid (*c).name(), c->getTitle().toRawUTF8()); }
    }
    check (controls >= 20 && untitled == 0, juce::String (controls) + " controls, each with an accessibility title");
    check (untipped == 0, "every control has a tooltip");

    editor.reset();
    folder.deleteRecursively();
    std::printf ("\n%s\n", failures == 0 ? "All plugin checks passed." : (juce::String (failures) + " plugin check(s) failed.").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
}

int main (int argc, char** argv)
{
    if (argc > 1 && juce::String (argv[1]) == "--check")
        return runChecks();
    juce::ScopedJuceInitialiser_GUI gui;
    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/screenshots");
    outDir.createDirectory();

    for (const juce::String theme : { "dark", "light" })
    {
        std::unique_ptr<SnaplineAudioProcessor> processor (static_cast<SnaplineAudioProcessor*> (createPluginFilter()));
        processor->setPlayConfigDetails (2, 2, 48000.0, 256);
        processor->prepareToPlay (48000.0, 256);
        processor->setCurrentProgram (2);
        processor->parameters.getParameter ("key")->setValueNotifyingHost (processor->parameters.getParameter ("key")->convertTo0to1 (0.0f));
        processor->parameters.getParameter ("scale")->setValueNotifyingHost (processor->parameters.getParameter ("scale")->convertTo0to1 (1.0f));
        processor->theme = theme;

        std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditor());
        editor->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        feed (*processor, 48000.0, 4.3);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (800);

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        const auto file = outDir.getChildFile ("snapline-" + theme + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat png;
        if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
        {
            std::fprintf (stderr, "Could not write %s\n", file.getFullPathName().toRawUTF8());
            return 1;
        }
        std::printf ("Wrote %s\n", file.getFullPathName().toRawUTF8());
        editor.reset();
    }
    return 0;
}
