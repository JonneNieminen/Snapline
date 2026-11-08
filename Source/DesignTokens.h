#pragma once
#include <JuceHeader.h>

/**
    Design tokens: the only place colours are defined. They follow Prism's flat
    macOS style (solid surfaces, hairline borders, one calm accent) in dark and
    light. The vocal colour is Prism's vocal stem colour; it marks corrected pitch.
    design/theme/tokens.css holds the same values for the README art; keep both in step.
*/
namespace snapline::theme
{
struct Palette
{
    juce::Colour background, sidebar, surface, elevated, pressed;
    juce::Colour border, borderStrong;
    juce::Colour text, muted, faint;
    juce::Colour accent, accentHover, accentText, accentSoft;
    juce::Colour vocal, vocalSoft;
    juce::Colour danger, success, warning;
    juce::Colour tooltipBackground, tooltipText;
    juce::Colour lane, laneActive;
    juce::Colour laneUpper, laneLower; // Prism's bass and drums stem colours (logo lanes)
    bool dark = true;
};

inline Palette darkPalette()
{
    Palette p;
    p.background = juce::Colour (0xff1c1c1e);
    p.sidebar = juce::Colour (0xff232325);
    p.surface = juce::Colour (0xff2a2a2d);
    p.elevated = juce::Colour (0xff333336);
    p.pressed = juce::Colour (0xff3c3c40);
    p.border = juce::Colour (0x17ffffff);
    p.borderStrong = juce::Colour (0x29ffffff);
    p.text = juce::Colour (0xffececee);
    p.muted = juce::Colour (0xff9a9aa1);
    p.faint = juce::Colour (0xff6c6c72);
    p.accent = juce::Colour (0xff7b82e0);
    p.accentHover = juce::Colour (0xff8a90e6);
    p.accentText = juce::Colour (0xffffffff);
    p.accentSoft = juce::Colour (0x297b82e0);
    p.vocal = juce::Colour (0xffe48aa0);
    p.vocalSoft = juce::Colour (0x33e48aa0);
    p.danger = juce::Colour (0xffe5787a);
    p.success = juce::Colour (0xff6cc08f);
    p.warning = juce::Colour (0xffd9a85b);
    p.tooltipBackground = juce::Colour (0xffe8e8ea);
    p.tooltipText = juce::Colour (0xff1c1c1e);
    p.lane = juce::Colour (0x08ffffff);
    p.laneActive = juce::Colour (0x147b82e0);
    p.laneUpper = juce::Colour (0xff9d92e4);
    p.laneLower = juce::Colour (0xffddb06a);
    p.dark = true;
    return p;
}

inline Palette lightPalette()
{
    Palette p;
    p.background = juce::Colour (0xfff5f5f7);
    p.sidebar = juce::Colour (0xffebebee);
    p.surface = juce::Colour (0xffffffff);
    p.elevated = juce::Colour (0xfff0f0f3);
    p.pressed = juce::Colour (0xffe6e6ea);
    p.border = juce::Colour (0x14000000);
    p.borderStrong = juce::Colour (0x26000000);
    p.text = juce::Colour (0xff1d1d1f);
    p.muted = juce::Colour (0xff6e6e73);
    p.faint = juce::Colour (0xffa1a1a6);
    p.accent = juce::Colour (0xff5a60c8);
    p.accentHover = juce::Colour (0xff4f55bd);
    p.accentText = juce::Colour (0xffffffff);
    p.accentSoft = juce::Colour (0x1c5a60c8);
    p.vocal = juce::Colour (0xffc95f78);
    p.vocalSoft = juce::Colour (0x29c95f78);
    p.danger = juce::Colour (0xffc94b4e);
    p.success = juce::Colour (0xff2f9663);
    p.warning = juce::Colour (0xffb07a1f);
    p.tooltipBackground = juce::Colour (0xff2c2c2e);
    p.tooltipText = juce::Colour (0xfff5f5f7);
    p.lane = juce::Colour (0x07000000);
    p.laneActive = juce::Colour (0x125a60c8);
    p.laneUpper = juce::Colour (0xff7166c4);
    p.laneLower = juce::Colour (0xffb8862f);
    p.dark = false;
    return p;
}

inline juce::Font uiFont (float size, bool semibold = false)
{
    return juce::Font (juce::FontOptions (size, semibold ? juce::Font::bold : juce::Font::plain));
}
}
