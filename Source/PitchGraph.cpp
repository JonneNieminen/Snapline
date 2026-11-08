#include "PitchGraph.h"

namespace
{
using snapline::theme::uiFont;

juce::String noteLabel (int midi)
{
    if (midi < 0)
        return juce::String::fromUTF8 ("\xe2\x80\x94"); // em dash
    return juce::String (snapline::noteNames[(size_t) (midi % 12)]) + juce::String (midi / 12 - 1);
}

juce::String centsText (float cents)
{
    const int rounded = juce::roundToInt (cents);
    if (rounded == 0)
        return "0 ct";
    return (rounded > 0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (rounded)) + " ct";
}
}

PitchGraph::PitchGraph() : history ((size_t) capacity)
{
    setTooltip ("Pitch over the last five seconds. Grey is what was sung, colour is what you hear. "
                "Highlighted lanes are the target notes.");
}

void PitchGraph::setNotes (snapline::NoteMask activeNotes, float referenceHz)
{
    if (activeNotes != notes || std::abs (referenceHz - reference) > 1.0e-4f)
    {
        notes = activeNotes;
        reference = referenceHz;
        repaint();
    }
}

void PitchGraph::push (const snapline::DisplayFrame& frame)
{
    history[(size_t) writeIndex] = frame;
    writeIndex = (writeIndex + 1) % capacity;
    stored = juce::jmin (stored + 1, capacity);
    if (frame.inputHz > 0.0f)
    {
        latest = frame;
        framesSinceVoiced = 0;
    }
    else
    {
        ++framesSinceVoiced;
        if (frame.targetNote < 0 && framesSinceVoiced > (int) (frameRate * 0.25))
            latest = {};
    }
}

const snapline::DisplayFrame& PitchGraph::at (int age) const
{
    return history[(size_t) ((writeIndex - 1 - age + capacity * 2) % capacity)];
}

void PitchGraph::advance (float seconds)
{
    // Follow the median of the recent output pitch so the singer stays in view.
    std::array<float, 128> recent {};
    int count = 0;
    const int span = juce::jmin (stored, (int) (frameRate * 1.5));
    for (int age = 0; age < span && count < (int) recent.size(); age += 3)
        if (at (age).outputHz > 0.0f)
            recent[(size_t) count++] = snapline::hzToMidi (at (age).outputHz, reference);
    if (count > 0)
    {
        std::nth_element (recent.begin(), recent.begin() + count / 2, recent.begin() + count);
        const float target = recent[(size_t) count / 2];
        const float coeff = 1.0f - std::exp (-seconds / 0.35f);
        if (std::abs (target - viewCentre) > 4.0f)
            viewCentre += (target - viewCentre) * coeff;
        else
            viewCentre += (target - viewCentre) * coeff * 0.25f;
    }
    repaint();
}

float PitchGraph::midiToY (float midi, juce::Rectangle<float> plot) const
{
    const float top = viewCentre + visibleSemitones * 0.5f;
    return plot.getY() + (top - midi) / visibleSemitones * plot.getHeight();
}

void PitchGraph::paint (juce::Graphics& g)
{
    if (colours == nullptr)
        return;
    const auto& c = *colours;
    auto bounds = getLocalBounds().toFloat();
    g.setColour (c.surface);
    g.fillRoundedRectangle (bounds, 12.0f);
    g.setColour (c.border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);

    auto inner = bounds.reduced (18.0f, 14.0f);
    paintHeader (g, inner.removeFromTop (58.0f));
    inner.removeFromTop (8.0f);
    auto gutter = inner.removeFromLeft (40.0f);
    paintLanes (g, inner, gutter);
    paintTraces (g, inner);
}

void PitchGraph::paintHeader (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto& c = *colours;
    const bool active = latest.targetNote >= 0 && latest.inputHz > 0.0f;

    // Current note
    auto noteArea = r.removeFromLeft (120.0f);
    g.setColour (active ? c.text : c.faint);
    g.setFont (uiFont (30.0f, true));
    g.drawText (noteLabel (latest.targetNote), noteArea.removeFromTop (36.0f), juce::Justification::centredLeft);
    g.setColour (c.muted);
    g.setFont (uiFont (11.0f));
    g.drawText ("Target note", noteArea, juce::Justification::centredLeft);

    // Input deviation
    auto inputArea = r.removeFromLeft (130.0f);
    const float deviation = active ? (snapline::hzToMidi (latest.inputHz, reference) - (float) latest.targetNote) * 100.0f : 0.0f;
    g.setColour (active ? c.text : c.faint);
    g.setFont (uiFont (20.0f, true));
    g.drawText (active ? centsText (deviation) : juce::String::fromUTF8 ("\xe2\x80\x94"), inputArea.removeFromTop (36.0f).withTrimmedTop (6.0f),
                juce::Justification::centredLeft);
    g.setColour (c.muted);
    g.setFont (uiFont (11.0f));
    g.drawText ("Sung, off the note", inputArea, juce::Justification::centredLeft);

    // Correction meter: how far the pitch is being moved right now (+-100 ct).
    auto meter = r.removeFromRight (juce::jmin (260.0f, r.getWidth()));
    auto label = meter.removeFromBottom (18.0f);
    auto bar = meter.withSizeKeepingCentre (meter.getWidth(), 8.0f).translated (0.0f, 4.0f);
    g.setColour (c.elevated);
    g.fillRoundedRectangle (bar, 4.0f);
    const float correction = active ? juce::jlimit (-100.0f, 100.0f, latest.correctionCents) : 0.0f;
    const float centreX = bar.getCentreX();
    const float endX = centreX + correction / 100.0f * bar.getWidth() * 0.5f;
    auto fill = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (centreX, endX), bar.getY(), juce::jmax (centreX, endX), bar.getBottom());
    g.setColour (c.vocal);
    g.fillRoundedRectangle (fill, 4.0f);
    g.setColour (c.borderStrong);
    g.fillRect (centreX - 0.5f, bar.getY() - 4.0f, 1.0f, bar.getHeight() + 8.0f);
    g.setColour (c.muted);
    g.setFont (uiFont (11.0f));
    g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92") + "100", label, juce::Justification::centredLeft);
    g.drawText ("+100", label, juce::Justification::centredRight);
    g.setColour (c.text);
    g.drawText ("Correction " + (active ? centsText (correction) : juce::String::fromUTF8 ("\xe2\x80\x94")), label, juce::Justification::centred);
}

void PitchGraph::paintLanes (juce::Graphics& g, juce::Rectangle<float> plot, juce::Rectangle<float> gutter)
{
    const auto& c = *colours;
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (plot.getUnion (gutter).toNearestInt());

    const float laneHeight = plot.getHeight() / visibleSemitones;
    const int lowest = (int) std::floor (viewCentre - visibleSemitones * 0.5f) - 1;
    const int highest = (int) std::ceil (viewCentre + visibleSemitones * 0.5f) + 1;
    for (int n = lowest; n <= highest; ++n)
    {
        if (n < 0 || n > 127)
            continue;
        const float y = midiToY ((float) n, plot);
        const auto lane = juce::Rectangle<float> (plot.getX(), y - laneHeight * 0.5f, plot.getWidth(), laneHeight);
        const bool allowed = (notes & (1u << (n % 12))) != 0;
        const bool current = latest.targetNote == n && latest.inputHz > 0.0f;
        if (current)
            g.setColour (c.accentSoft);
        else
            g.setColour (allowed ? c.laneActive : c.lane);
        g.fillRect (lane.reduced (0.0f, 0.5f));

        if (allowed || n % 12 == 0)
        {
            g.setColour (current ? c.accent : (allowed ? c.muted : c.faint));
            g.setFont (uiFont (10.5f, current));
            g.drawText (noteLabel (n), gutter.withY (y - 8.0f).withHeight (16.0f).toNearestInt(), juce::Justification::centredLeft);
        }
    }
    g.setColour (c.border);
    g.drawVerticalLine ((int) plot.getRight() - 1, plot.getY(), plot.getBottom());
}

void PitchGraph::paintTraces (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const auto& c = *colours;
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (plot.toNearestInt());

    const int visibleFrames = juce::jmin (stored, (int) (frameRate * visibleSeconds));
    const float pixelsPerFrame = plot.getWidth() / (float) (frameRate * visibleSeconds);

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool output = pass == 1;
        juce::Path path;
        bool drawing = false;
        float lastMidi = 0.0f;
        for (int age = visibleFrames - 1; age >= 0; --age)
        {
            const auto& f = at (age);
            const float hz = output ? f.outputHz : f.inputHz;
            if (hz <= 0.0f)
            {
                drawing = false;
                continue;
            }
            const float midi = snapline::hzToMidi (hz, reference);
            const float x = plot.getRight() - (float) age * pixelsPerFrame;
            const float y = midiToY (midi, plot);
            if (! drawing || std::abs (midi - lastMidi) > 3.0f)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
            drawing = true;
            lastMidi = midi;
        }
        g.setColour (output ? c.vocal : c.faint);
        g.strokePath (path, juce::PathStrokeType (output ? 2.2f : 1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (framesSinceVoiced > (int) (frameRate * visibleSeconds))
    {
        g.setColour (c.muted);
        g.setFont (uiFont (13.0f));
        g.drawText ("Play or sing into the track to see its pitch", plot, juce::Justification::centred);
    }
}
