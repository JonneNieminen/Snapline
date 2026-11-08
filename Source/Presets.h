#pragma once
#include <array>

namespace snapline
{
/**
    Factory presets set the correction style only. Key, scale, input type, mode,
    MIDI, mix and output stay as they are, so a preset never retunes a song.
*/
struct PresetValues
{
    float retuneMs, humanize, tolerance, amount;
    float vibratoDepth, vibratoRate, vibratoOnsetMs;
    float naturalVibrato; // -100 flattens the singer's own vibrato, 0 keeps it, +100 doubles it
    bool preserveFormants;
    float formantShift, throat;
};

struct FactoryPreset
{
    const char* name;
    const char* description;
    PresetValues values;
};

inline const std::array<FactoryPreset, 7> factoryPresets {{
    { "Natural",       "Transparent correction that keeps scoops, vibrato and expression.",
      { 85.0f, 45.0f, 35.0f, 100.0f, 0.0f, 5.5f, 250.0f, 0.0f, true, 0.0f, 100.0f } },
    { "Gentle Polish", "Light, quick-acting tuning for a finished but human vocal.",
      { 45.0f, 30.0f, 15.0f, 100.0f, 0.0f, 5.5f, 250.0f, 0.0f, true, 0.0f, 100.0f } },
    { "Pop",           "Tight, modern pop tuning that still breathes on long notes.",
      { 18.0f, 20.0f, 0.0f, 100.0f, 0.0f, 5.5f, 250.0f, 0.0f, true, 0.0f, 100.0f } },
    { "Hard Tune",     "Instant snapping between notes: the classic stepped effect.",
      { 0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 5.5f, 250.0f, -100.0f, true, 0.0f, 100.0f } },
    { "Robot",         "Hard tune with formants that follow the pitch steps.",
      { 0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 5.5f, 250.0f, -100.0f, false, 0.0f, 100.0f } },
    { "Ballad",        "Smooth correction with a gentle vibrato on held notes.",
      { 60.0f, 50.0f, 10.0f, 100.0f, 16.0f, 5.2f, 350.0f, 0.0f, true, 0.0f, 100.0f } },
    { "Deep Voice",    "Medium correction with a longer throat for a darker character.",
      { 30.0f, 25.0f, 0.0f, 100.0f, 0.0f, 5.5f, 250.0f, 0.0f, true, -2.0f, 115.0f } },
}};
}
