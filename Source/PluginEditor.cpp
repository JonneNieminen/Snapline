#include "PluginEditor.h"

namespace
{
using snapline::theme::uiFont;
constexpr int editorWidth = 1080;
constexpr int editorHeight = 760;
constexpr int sidebarWidth = 248;

const juce::String dot = juce::String::fromUTF8 (" \xc2\xb7 ");

float textWidth (const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (font, text);
}
}

// ============================================================================ look and feel
void SnaplineLookAndFeel::setPalette (const snapline::theme::Palette& p)
{
    colours = p;
    setColour (juce::ComboBox::textColourId, p.text);
    setColour (juce::ComboBox::backgroundColourId, p.elevated);
    setColour (juce::ComboBox::outlineColourId, p.border);
    setColour (juce::ComboBox::arrowColourId, p.muted);
    setColour (juce::PopupMenu::backgroundColourId, p.surface);
    setColour (juce::PopupMenu::textColourId, p.text);
    setColour (juce::PopupMenu::headerTextColourId, p.muted);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, p.accent);
    setColour (juce::PopupMenu::highlightedTextColourId, p.accentText);
    setColour (juce::Label::textColourId, p.text);
    setColour (juce::TooltipWindow::textColourId, p.tooltipText);
    setColour (juce::TooltipWindow::backgroundColourId, p.tooltipBackground);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colours::transparentBlack);
}

void SnaplineLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                            float, float, juce::Slider::SliderStyle, juce::Slider& slider)
{
    const auto& c = colours;
    const float cy = (float) y + (float) height * 0.5f;
    const float left = (float) x + 7.0f, right = (float) (x + width) - 7.0f;
    const auto track = juce::Rectangle<float>::leftTopRightBottom (left, cy - 2.0f, right, cy + 2.0f);
    g.setColour (c.pressed);
    g.fillRoundedRectangle (track, 2.0f);

    // Bipolar controls fill from zero.
    float origin = left;
    const auto range = slider.getRange();
    if (range.getStart() < 0.0 && range.getEnd() > 0.0)
        origin = left + (right - left) * (float) slider.valueToProportionOfLength (0.0);
    const float thumbX = juce::jlimit (left, right, sliderPos);
    g.setColour (slider.isEnabled() ? c.accent : c.faint);
    g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (origin, thumbX), cy - 2.0f,
                                                                       juce::jmax (origin, thumbX), cy + 2.0f), 2.0f);

    const auto thumb = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ thumbX, cy });
    g.setColour (juce::Colour (0x33000000));
    g.fillEllipse (thumb.translated (0.0f, 1.0f).expanded (0.5f));
    g.setColour (juce::Colours::white);
    g.fillEllipse (thumb);
    g.setColour (c.dark ? juce::Colour (0x22000000) : juce::Colour (0x26000000));
    g.drawEllipse (thumb.reduced (0.25f), 0.5f);
    if (slider.isMouseOverOrDragging())
    {
        g.setColour (c.accentSoft);
        g.drawEllipse (thumb.expanded (3.0f), 3.0f);
    }
}

void SnaplineLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool down, int, int, int, int, juce::ComboBox& box)
{
    const auto& c = colours;
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (down ? c.pressed : (box.isMouseOver (true) ? c.pressed : c.elevated));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (box.hasKeyboardFocus (true) ? c.accent : c.border);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    juce::Path chevron;
    const float cx = (float) width - 15.0f, cy = (float) height * 0.5f;
    chevron.startNewSubPath (cx - 3.5f, cy - 1.8f);
    chevron.lineTo (cx, cy + 1.8f);
    chevron.lineTo (cx + 3.5f, cy - 1.8f);
    g.setColour (c.muted);
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font SnaplineLookAndFeel::getComboBoxFont (juce::ComboBox&) { return uiFont (13.0f); }
juce::Font SnaplineLookAndFeel::getPopupMenuFont() { return uiFont (13.0f); }

void SnaplineLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 0, box.getWidth() - 30, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

void SnaplineLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (colours.surface);
    g.setColour (colours.borderStrong);
    g.drawRect (0, 0, width, height);
}

void SnaplineLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // macOS-style switch on the right, label on the left.
    const auto& c = colours;
    auto r = button.getLocalBounds().toFloat();
    const bool on = button.getToggleState();
    auto trackArea = r.removeFromRight (32.0f).withSizeKeepingCentre (32.0f, 18.0f);
    g.setColour (on ? c.accent : c.pressed);
    g.fillRoundedRectangle (trackArea, 9.0f);
    if (highlighted && ! on)
    {
        g.setColour (c.borderStrong);
        g.drawRoundedRectangle (trackArea.reduced (0.5f), 9.0f, 1.0f);
    }
    const auto knob = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ on ? trackArea.getRight() - 9.0f : trackArea.getX() + 9.0f, trackArea.getCentreY() });
    g.setColour (juce::Colour (0x33000000));
    g.fillEllipse (knob.translated (0.0f, 0.75f));
    g.setColour (juce::Colours::white);
    g.fillEllipse (knob);
    g.setColour (c.text);
    g.setFont (uiFont (12.5f));
    g.drawText (button.getButtonText(), r.withTrimmedRight (8.0f), juce::Justification::centredRight);
}

void SnaplineLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (colours.tooltipBackground);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (colours.tooltipText);
    g.setFont (uiFont (12.0f));
    g.drawFittedText (text, r.reduced (10.0f, 6.0f).toNearestInt(), juce::Justification::centredLeft, 4);
}

juce::Rectangle<int> SnaplineLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto font = uiFont (12.0f);
    const int maxWidth = 300;
    const int textW = (int) std::ceil (textWidth (font, tipText));
    const int lines = juce::jmax (1, (int) std::ceil ((float) textW / (float) (maxWidth - 20)));
    const int w = juce::jmin (maxWidth, textW + 22);
    const int h = (int) std::ceil (font.getHeight() * 1.25f) * lines + 12;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 18, w, h)
        .constrainedWithin (parentArea);
}

// ============================================================================ segmented
Segmented::Segmented (juce::StringArray options) : labels (std::move (options)) {}

void Segmented::setSelected (int index, bool notify)
{
    index = juce::jlimit (0, labels.size() - 1, index);
    if (index == selected)
        return;
    selected = index;
    repaint();
    if (notify && onChange)
        onChange (selected);
}

void Segmented::paint (juce::Graphics& g)
{
    if (colours == nullptr)
        return;
    const auto& c = *colours;
    auto r = getLocalBounds().toFloat();
    g.setColour (c.elevated);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (c.border);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
    const float w = r.getWidth() / (float) labels.size();
    for (int i = 0; i < labels.size(); ++i)
    {
        auto cell = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (3.0f);
        if (i == selected)
        {
            g.setColour (c.dark ? c.pressed.brighter (0.08f) : c.surface);
            g.fillRoundedRectangle (cell, 6.0f);
            g.setColour (c.borderStrong);
            g.drawRoundedRectangle (cell.reduced (0.5f), 6.0f, 0.75f);
        }
        g.setColour (i == selected ? c.text : c.muted);
        g.setFont (uiFont (12.5f, i == selected));
        g.drawText (labels[i], cell, juce::Justification::centred);
    }
}

void Segmented::mouseDown (const juce::MouseEvent& e)
{
    const int index = (int) ((float) e.x / ((float) getWidth() / (float) labels.size()));
    setSelected (index, true);
}

// ============================================================================ parameter row
ParameterRow::ParameterRow (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                            const juce::String& name, const juce::String& tooltip)
    : label (name), attachment (state, parameterId, slider)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    slider.setScrollWheelEnabled (false);
    slider.setTooltip (tooltip);
    if (auto* parameter = state.getParameter (parameterId))
        slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
}

void ParameterRow::resized()
{
    auto r = getLocalBounds();
    r.removeFromLeft (108);
    r.removeFromRight (70);
    slider.setBounds (r);
}

void ParameterRow::paint (juce::Graphics& g)
{
    if (colours == nullptr)
        return;
    auto r = getLocalBounds().toFloat();
    g.setFont (uiFont (13.0f));
    g.setColour (colours->text);
    g.drawText (label, r.removeFromLeft (108.0f), juce::Justification::centredLeft);
    g.setColour (colours->muted);
    g.setFont (uiFont (12.5f));
    g.drawText (slider.getTextFromValue (slider.getValue()), r.removeFromRight (66.0f), juce::Justification::centredRight);
}

// ============================================================================ note strip
NoteStrip::NoteStrip()
{
    setTooltip ("Target notes. Click a key to add or remove it; this switches the scale to Custom. "
                "Coloured: the note you are on. Outlined: notes held on a MIDI keyboard.");
}

void NoteStrip::setState (snapline::NoteMask a, snapline::NoteMask h, int currentPitchClass)
{
    if (a != active || h != held || currentPitchClass != current)
    {
        active = a;
        held = h;
        current = currentPitchClass;
        repaint();
    }
}

namespace
{
constexpr int whiteKeys[] { 0, 2, 4, 5, 7, 9, 11 };
bool isBlack (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
}

juce::Rectangle<float> NoteStrip::keyBounds (int pc) const
{
    const auto r = getLocalBounds().toFloat();
    const float ww = r.getWidth() / 7.0f;
    if (! isBlack (pc))
    {
        int index = 0;
        for (int i = 0; i < 7; ++i)
            if (whiteKeys[i] == pc) index = i;
        return { r.getX() + ww * (float) index, r.getY(), ww, r.getHeight() };
    }
    const int whiteBefore = pc == 1 ? 1 : pc == 3 ? 2 : pc == 6 ? 4 : pc == 8 ? 5 : 6;
    const float bw = ww * 0.62f;
    return { r.getX() + ww * (float) whiteBefore - bw * 0.5f, r.getY(), bw, r.getHeight() * 0.58f };
}

int NoteStrip::keyAt (juce::Point<float> p) const
{
    for (int pc : { 1, 3, 6, 8, 10 })
        if (keyBounds (pc).contains (p)) return pc;
    for (int pc : whiteKeys)
        if (keyBounds (pc).contains (p)) return pc;
    return -1;
}

void NoteStrip::paint (juce::Graphics& g)
{
    if (colours == nullptr)
        return;
    const auto& c = *colours;
    const auto draw = [&] (int pc)
    {
        const bool black = isBlack (pc);
        const bool on = (active & (1u << pc)) != 0;
        auto k = keyBounds (pc).reduced (1.0f, 0.0f);
        juce::Colour fill;
        if (pc == current) fill = c.vocal;
        else if (on) fill = black ? c.accent : c.accentSoft.withMultipliedAlpha (1.6f);
        else fill = black ? c.pressed : c.elevated;
        if (! black && ! on && pc != current)
            fill = c.surface;
        g.setColour (fill);
        g.fillRoundedRectangle (k, 4.0f);
        g.setColour (black ? c.borderStrong : c.border);
        g.drawRoundedRectangle (k.reduced (0.5f), 4.0f, 1.0f);
        if ((held & (1u << pc)) != 0)
        {
            g.setColour (c.vocal);
            g.drawRoundedRectangle (k.reduced (1.5f), 4.0f, 2.0f);
        }
        if (! black)
        {
            g.setColour (pc == current ? c.accentText : (on ? c.text : c.faint));
            g.setFont (uiFont (10.5f, on));
            g.drawText (snapline::noteNames[(size_t) pc], k.removeFromBottom (18.0f), juce::Justification::centred);
        }
    };
    for (int pc : whiteKeys) draw (pc);
    for (int pc : { 1, 3, 6, 8, 10 }) draw (pc);
}

void NoteStrip::mouseDown (const juce::MouseEvent& e)
{
    const int pc = keyAt (e.position);
    if (pc >= 0 && onToggle)
        onToggle (pc);
}

// ============================================================================ editor
SnaplineAudioProcessorEditor::SnaplineAudioProcessorEditor (SnaplineAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      retune (p.parameters, "retune", "Retune speed", "How quickly notes are pulled to pitch. 0 ms snaps instantly (the hard-tune effect); 50-100 ms sounds natural."),
      humanize (p.parameters, "humanize", "Humanize", "Lets long, sustained notes relax so they keep their natural movement, while short notes stay tight."),
      tolerance (p.parameters, "tolerance", "Tolerance", "Only corrects notes the singer is already close to. Higher values leave more of the performance untouched."),
      amount (p.parameters, "amount", "Amount", "How much of the correction is applied, from none to all of it."),
      vibratoDepth (p.parameters, "vibratoDepth", "Depth", "Adds vibrato to held notes, in cents either side of the note."),
      vibratoRate (p.parameters, "vibratoRate", "Rate", "Vibrato speed."),
      vibratoOnset (p.parameters, "vibratoOnset", "Onset", "How long a note is held before the vibrato fades in."),
      naturalVibrato (p.parameters, "naturalVibrato", "Natural", "The singer's own vibrato. 0 % keeps it as sung, -100 % flattens it, +100 % doubles it."),
      formantShift (p.parameters, "formantShift", "Formant", "Moves the voice's character (its formants) up or down without changing the pitch."),
      throat (p.parameters, "throat", "Throat", "Models a longer (darker) or shorter (brighter) throat. 100 % is natural."),
      transpose (p.parameters, "transpose", "Transpose", "Shifts the corrected pitch by semitones."),
      mix (p.parameters, "mix", "Mix", "Blend of the corrected and the original vocal."),
      output (p.parameters, "output", "Output", "Output level."),
      reference (p.parameters, "reference", "A4 tuning", "Reference pitch for A4. Use it when the track isn't tuned to 440 Hz.")
{
    colours = audioProcessor.theme == "light" ? snapline::theme::lightPalette() : snapline::theme::darkPalette();
    lookAndFeel.setPalette (colours);
    setLookAndFeel (&lookAndFeel);
    setSize (editorWidth, editorHeight);

    for (int i = 0; i < SnaplineAudioProcessor::numPresets; ++i)
        preset.addItem (snapline::factoryPresets[(size_t) i].name, i + 1);
    preset.setTextWhenNothingSelected ("Custom");
    preset.onChange = [this]
    {
        if (preset.getSelectedId() > 0)
            audioProcessor.setCurrentProgram (preset.getSelectedId() - 1);
    };
    configureCombo (preset, "Load a correction style. Key, scale and input settings stay as they are.");

    for (auto* name : snapline::noteNames) key.addItem (name, key.getNumItems() + 1);
    for (const auto& s : snapline::scales) scale.addItem (s.name, scale.getNumItems() + 1);
    inputType.addItemList ({ "Soprano", "Alto / Tenor", "Low Male", "Instrument" }, 1);
    midiMode.addItemList ({ "Off", "Snap to held notes", "Force to held note" }, 1);
    configureCombo (key, "The song's key.");
    configureCombo (scale, "Notes the vocal is tuned to. Chromatic allows every note; Custom uses the keys you pick below.");
    configureCombo (inputType, "The voice's range. It sets where pitch is searched for, which avoids octave mistakes.");
    configureCombo (midiMode, "Use MIDI from the track: snap to the notes you hold, or force the vocal to the note you hold.");

    auto& state = audioProcessor.parameters;
    keyAttachment = std::make_unique<ComboAttachment> (state, "key", key);
    scaleAttachment = std::make_unique<ComboAttachment> (state, "scale", scale);
    inputTypeAttachment = std::make_unique<ComboAttachment> (state, "inputType", inputType);
    midiModeAttachment = std::make_unique<ComboAttachment> (state, "midiMode", midiMode);

    noteStrip.onToggle = [this] (int pc) { audioProcessor.toggleNote (pc); };
    addAndMakeVisible (noteStrip);

    graph.setFrameRate (audioProcessor.getDisplayFrameRate());
    addAndMakeVisible (graph);

    modeSwitch.setTooltip ("Studio: highest quality with a fixed 100 ms delay your DAW compensates for. "
                           "Live: low delay for monitoring while you record.");
    if (auto* modeParameter = state.getParameter ("mode"))
    {
        modeAttachment = std::make_unique<juce::ParameterAttachment> (*modeParameter, [this] (float v)
        {
            modeSwitch.setSelected (juce::roundToInt (v), false);
        });
        modeSwitch.onChange = [this] (int i) { modeAttachment->setValueAsCompleteGesture ((float) i); };
        modeAttachment->sendInitialUpdate();
    }
    addAndMakeVisible (modeSwitch);

    themeSwitch.setSelected (audioProcessor.theme == "light" ? 1 : 0, false);
    themeSwitch.setTooltip ("Appearance");
    themeSwitch.onChange = [this] (int i)
    {
        audioProcessor.theme = i == 1 ? "light" : "dark";
        applyTheme();
    };
    addAndMakeVisible (themeSwitch);

    preserveFormants.setTooltip ("Keeps the voice's natural character when the pitch moves. Turn off for a more synthetic, chipmunk-style shift.");
    preserveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "preserveFormants", preserveFormants);
    addAndMakeVisible (preserveFormants);

    for (auto* row : { &retune, &humanize, &tolerance, &amount, &vibratoDepth, &vibratoRate, &vibratoOnset, &naturalVibrato,
                       &formantShift, &throat, &transpose, &mix, &output, &reference })
        addAndMakeVisible (row);

    applyTheme();
    startTimerHz (30);
}

SnaplineAudioProcessorEditor::~SnaplineAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SnaplineAudioProcessorEditor::configureCombo (juce::ComboBox& box, const juce::String& tooltip)
{
    box.setJustificationType (juce::Justification::centredLeft);
    box.setTooltip (tooltip);
    addAndMakeVisible (box);
}

void SnaplineAudioProcessorEditor::applyTheme()
{
    colours = audioProcessor.theme == "light" ? snapline::theme::lightPalette() : snapline::theme::darkPalette();
    lookAndFeel.setPalette (colours);
    graph.setPalette (&colours);
    noteStrip.setPalette (&colours);
    modeSwitch.setPalette (&colours);
    themeSwitch.setPalette (&colours);
    for (auto* row : { &retune, &humanize, &tolerance, &amount, &vibratoDepth, &vibratoRate, &vibratoOnset, &naturalVibrato,
                       &formantShift, &throat, &transpose, &mix, &output, &reference })
        row->setPalette (&colours);
    sendLookAndFeelChange();
    repaint();
}

juce::String SnaplineAudioProcessorEditor::scaleSummary() const
{
    const int midi = midiMode.getSelectedId() - 1;
    if (midi == 2) return "MIDI: force to held note";
    if (midi == 1 && audioProcessor.getHeldMidiNotes() != 0) return "MIDI: held notes";
    if (scale.getSelectedId() - 1 == snapline::customScaleIndex) return "Custom notes";
    if (scale.getSelectedId() == 1) return "Chromatic";
    return key.getText() + " " + scale.getText();
}

void SnaplineAudioProcessorEditor::paintWordmark (juce::Graphics& g, juce::Point<float> origin)
{
    // Mark ("Snap", see design/logo/make.py): a wobbling sung line that settles onto
    // the vocal lane, between two quieter note lanes. Same geometry as the SVG masters.
    const auto t = juce::AffineTransform::scale (30.0f / 64.0f).translated (origin.x - 2.0f, origin.y - 2.0f);
    const juce::PathStrokeType stroke (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const auto lane = [&] (const char* d, float y, juce::Colour c, float dotRadius)
    {
        g.setColour (c);
        g.strokePath (juce::Drawable::parseSVGPath (d), stroke, t);
        g.fillEllipse (juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f).withCentre ({ 49.0f, y }).transformedBy (t));
    };
    lane ("M35 19H49", 19.0f, colours.laneUpper.interpolatedWith (colours.sidebar, 0.5f), 3.0f);
    lane ("M35 45H49", 45.0f, colours.laneLower.interpolatedWith (colours.sidebar, 0.5f), 3.0f);
    lane ("M33 32H49", 32.0f, colours.vocal, 3.6f);
    g.setColour (colours.text);
    g.strokePath (juce::Drawable::parseSVGPath ("M10 32C11.5 22 16.5 22 18 32S23.5 39 25 32S28.8 29 30 32S32 32.8 33 32"), stroke, t);

    // Lowercase wordmark whose i-dot is the accent, as in Prism.
    const auto font = uiFont (18.0f, true);
    const float x = origin.x + 36.0f, baselineY = origin.y + 19.0f;
    g.setFont (font);
    g.setColour (colours.text);
    const juce::String before = "snapl", stem = juce::String::fromUTF8 ("\xc4\xb1"), after = "ne";
    g.drawSingleLineText (before, (int) x, (int) baselineY);
    const float stemX = x + textWidth (font, before);
    g.drawSingleLineText (stem, (int) stemX, (int) baselineY);
    const float stemW = textWidth (font, stem);
    g.drawSingleLineText (after, (int) (stemX + stemW), (int) baselineY);
    g.setColour (colours.accent);
    g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ stemX + stemW * 0.5f, baselineY - font.getAscent() * 0.86f }));
}

void SnaplineAudioProcessorEditor::paintCard (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
{
    const auto rf = r.toFloat();
    g.setColour (colours.surface);
    g.fillRoundedRectangle (rf, 12.0f);
    g.setColour (colours.border);
    g.drawRoundedRectangle (rf.reduced (0.5f), 12.0f, 1.0f);
    g.setColour (colours.text);
    g.setFont (uiFont (14.0f, true));
    g.drawText (title, r.reduced (18, 0).withTop (r.getY() + 12).withHeight (24), juce::Justification::centredLeft);
}

void SnaplineAudioProcessorEditor::paintHeader (juce::Graphics& g)
{
    auto r = header;
    g.setColour (colours.text);
    g.setFont (uiFont (24.0f, true));
    g.drawText (headerTitle, r.removeFromTop (34), juce::Justification::centredLeft);

    auto line = r.removeFromTop (30).withTrimmedTop (6);
    g.setFont (uiFont (12.5f));
    for (const auto& chip : chips)
    {
        const int w = (int) std::ceil (textWidth (uiFont (12.5f), chip)) + 18;
        auto box = line.removeFromLeft (w).toFloat();
        g.setColour (colours.elevated);
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (colours.muted);
        g.drawText (chip, box, juce::Justification::centred);
        line.removeFromLeft (8);
    }
    g.setColour (colours.faint);
    g.drawText (headerSubtitle, line.toFloat(), juce::Justification::centredLeft);

    // Output meter under the mode switch.
    const auto meter = juce::Rectangle<float> ((float) modeSwitch.getX(), (float) modeSwitch.getBottom() + 14.0f, (float) modeSwitch.getWidth(), 4.0f);
    const float peak = audioProcessor.outputPeak.load (std::memory_order_relaxed);
    const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
    const float level = juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
    g.setColour (colours.pressed);
    g.fillRoundedRectangle (meter, 2.0f);
    g.setColour (db > -1.0f ? colours.danger : colours.success);
    g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * level), 2.0f);
}

void SnaplineAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours.background);
    g.setColour (colours.sidebar);
    g.fillRect (sidebar);
    g.setColour (colours.border);
    g.drawVerticalLine (sidebar.getRight() - 1, 0.0f, (float) getHeight());

    paintWordmark (g, { 22.0f, 24.0f });

    const auto sectionLabel = [&] (const juce::String& text, juce::Component& below)
    {
        g.setColour (colours.muted);
        g.setFont (uiFont (11.5f, true));
        g.drawText (text, juce::Rectangle<int> (below.getX(), below.getY() - 20, sidebar.getWidth() - 40, 16), juce::Justification::centredLeft);
    };
    sectionLabel ("Preset", preset);
    sectionLabel ("Key and scale", key);
    sectionLabel ("Target notes", noteStrip);
    sectionLabel ("Input type", inputType);
    sectionLabel ("MIDI notes", midiMode);

    g.setColour (colours.faint);
    g.setFont (uiFont (11.5f));
    g.drawFittedText ("Pitch tracking and correction run locally, in real time.",
                      juce::Rectangle<int> (22, getHeight() - 52, sidebar.getWidth() - 44, 34), juce::Justification::topLeft, 2);

    paintHeader (g);
    paintCard (g, cards[0], "Correction");
    paintCard (g, cards[1], "Vibrato");
    paintCard (g, cards[2], "Character");
    paintCard (g, cards[3], "Output");
}

void SnaplineAudioProcessorEditor::resized()
{
    sidebar = { 0, 0, sidebarWidth, getHeight() };
    const int sx = 20, sw = sidebarWidth - 40;
    preset.setBounds (sx, 92, sw, 30);
    key.setBounds (sx, 156, 72, 30);
    scale.setBounds (sx + 80, 156, sw - 80, 30);
    noteStrip.setBounds (sx, 220, sw, 84);
    inputType.setBounds (sx, 340, sw, 30);
    midiMode.setBounds (sx, 404, sw, 30);
    themeSwitch.setBounds (sx, getHeight() - 100, sw, 30);

    const int mx = sidebarWidth + 28, mw = getWidth() - mx - 28;
    header = { mx, 20, mw - 200, 66 };
    modeSwitch.setBounds (mx + mw - 180, 24, 180, 32);
    graph.setBounds (mx, 100, mw, 300);

    const int cw = (mw - 14) / 2, ch = 162, cy = 414;
    cards[0] = { mx, cy, cw, ch };
    cards[1] = { mx + cw + 14, cy, cw, ch };
    cards[2] = { mx, cy + ch + 12, cw, ch };
    cards[3] = { mx + cw + 14, cy + ch + 12, cw, ch };

    const auto layoutRows = [] (juce::Rectangle<int> card, std::initializer_list<ParameterRow*> rows)
    {
        int y = card.getY() + 46;
        for (auto* row : rows)
        {
            row->setBounds (card.getX() + 18, y, card.getWidth() - 36, 26);
            y += 28;
        }
    };
    layoutRows (cards[0], { &retune, &humanize, &tolerance, &amount });
    layoutRows (cards[1], { &vibratoDepth, &vibratoRate, &vibratoOnset, &naturalVibrato });
    layoutRows (cards[2], { &formantShift, &throat, &transpose });
    layoutRows (cards[3], { &mix, &output, &reference });
    preserveFormants.setBounds (cards[2].getRight() - 18 - 170, cards[2].getY() + 12, 170, 24);
}

void SnaplineAudioProcessorEditor::timerCallback()
{
    int n = 0;
    int lastTarget = -2;
    while ((n = audioProcessor.popDisplayFrames (frameBuffer.data(), (int) frameBuffer.size())) > 0)
        for (int i = 0; i < n; ++i)
        {
            graph.push (frameBuffer[(size_t) i]);
            lastTarget = frameBuffer[(size_t) i].targetNote;
        }

    const auto active = audioProcessor.getActiveNotes();
    const float referenceHz = audioProcessor.parameters.getRawParameterValue ("reference")->load();
    graph.setNotes (active, referenceHz);
    graph.advance (1.0f / 30.0f);
    if (lastTarget != -2)
        currentPitchClass = lastTarget >= 0 ? lastTarget % 12 : -1;
    noteStrip.setState (active, audioProcessor.getHeldMidiNotes(), currentPitchClass);

    const int selected = audioProcessor.presetModified.load() ? 0 : audioProcessor.getCurrentProgram() + 1;
    if (preset.getSelectedId() != selected)
        preset.setSelectedId (selected, juce::dontSendNotification);

    // Header text
    const auto title = selected > 0 ? juce::String (snapline::factoryPresets[(size_t) selected - 1].name) : juce::String ("Custom");
    const auto subtitle = selected > 0 ? juce::String (snapline::factoryPresets[(size_t) selected - 1].description)
                                       : juce::String ("Your own correction settings");
    const bool studio = modeSwitch.getSelected() == 1;
    const float delay = audioProcessor.getEffectiveDelayMs();
    if (std::abs (delay - displayedDelayMs) > 0.75f)
        displayedDelayMs = delay;
    juce::StringArray nextChips { scaleSummary(), inputType.getText(),
                                  (studio ? "Studio" : "Live") + dot + juce::String (juce::roundToInt (displayedDelayMs)) + " ms" };
    if (title != headerTitle || subtitle != headerSubtitle || nextChips != chips)
    {
        headerTitle = title;
        headerSubtitle = subtitle;
        chips = nextChips;
    }
    repaint (header.getUnion ({ modeSwitch.getX(), modeSwitch.getBottom(), modeSwitch.getWidth(), 24 }));
}
