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

int main (int argc, char** argv)
{
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
