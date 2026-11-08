#pragma once
#include <array>
#include <cstdint>

namespace snapline
{
/** Note sets are 12-bit masks of pitch classes: bit 0 = C, bit 1 = C#, ... bit 11 = B. */
using NoteMask = std::uint16_t;
constexpr NoteMask allNotes = 0x0fff;

struct ScaleDefinition
{
    const char* name;
    NoteMask intervals; // relative to the key, bit 0 = root
};

constexpr NoteMask bits (std::initializer_list<int> steps)
{
    NoteMask m = 0;
    for (int s : steps)
        m = (NoteMask) (m | (1u << s));
    return m;
}

/** The last entry, Custom, takes its notes from the twelve note parameters. */
inline const std::array<ScaleDefinition, 13> scales {{
    { "Chromatic",        bits ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }) },
    { "Major",            bits ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "Minor",            bits ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "Harmonic Minor",   bits ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "Melodic Minor",    bits ({ 0, 2, 3, 5, 7, 9, 11 }) },
    { "Major Pentatonic", bits ({ 0, 2, 4, 7, 9 }) },
    { "Minor Pentatonic", bits ({ 0, 3, 5, 7, 10 }) },
    { "Blues",            bits ({ 0, 3, 5, 6, 7, 10 }) },
    { "Dorian",           bits ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "Phrygian",         bits ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "Lydian",           bits ({ 0, 2, 4, 6, 7, 9, 11 }) },
    { "Mixolydian",       bits ({ 0, 2, 4, 5, 7, 9, 10 }) },
    { "Custom",           allNotes },
}};

constexpr int customScaleIndex = 12;

inline const std::array<const char*, 12> noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/** Rotates a scale's intervals onto a key (0 = C). */
constexpr NoteMask maskForKey (NoteMask intervals, int key)
{
    NoteMask m = 0;
    for (int i = 0; i < 12; ++i)
        if (intervals & (1u << i))
            m = (NoteMask) (m | (1u << ((i + key) % 12)));
    return m;
}
}
