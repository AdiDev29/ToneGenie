#include "Widgets.h"

juce::Font uiFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
GenieLookAndFeel::GenieLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Theme::background);
    setColour (juce::Label::textColourId, Theme::text);
    setColour (juce::Slider::textBoxTextColourId, Theme::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Theme::accentDark);
    setColour (juce::TextEditor::backgroundColourId, Theme::panel);
    setColour (juce::TextEditor::textColourId, Theme::text);
    setColour (juce::TextEditor::highlightColourId, Theme::accentDark);
    setColour (juce::TextEditor::outlineColourId, Theme::panelEdge);
    setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
    setColour (juce::CaretComponent::caretColourId, Theme::accent);
    setColour (juce::TextButton::buttonColourId, Theme::panelEdge);
    setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    setColour (juce::TextButton::textColourOffId, Theme::text);
    setColour (juce::TextButton::textColourOnId, Theme::background);
    setColour (juce::ComboBox::backgroundColourId, Theme::panelEdge);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::textColourId, Theme::text);
    setColour (juce::ComboBox::arrowColourId, Theme::dim);
    setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
    setColour (juce::PopupMenu::textColourId, Theme::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, Theme::background);
    setColour (juce::ToggleButton::textColourId, Theme::text);
    setColour (juce::ToggleButton::tickColourId, Theme::accent);
    setColour (juce::ToggleButton::tickDisabledColourId, Theme::dim);
}

void GenieLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const float track = juce::jmax (3.0f, radius * 0.14f);
    const float arcRadius = radius - track * 0.5f;
    const bool enabled = slider.isEnabled();

    juce::Path background;
    background.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Theme::panelEdge);
    g.strokePath (background, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, angle, true);
    g.setColour (enabled ? Theme::accent : Theme::dim);
    g.strokePath (value, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float capRadius = arcRadius - track * 1.3f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3f4a), centre.x, centre.y - capRadius,
                                             juce::Colour (0xff23262d), centre.x, centre.y + capRadius, false));
    g.fillEllipse (juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre));

    const juce::Point<float> tip (centre.x + (capRadius - 3.0f) * std::sin (angle), centre.y - (capRadius - 3.0f) * std::cos (angle));
    const juce::Point<float> base (centre.x + capRadius * 0.35f * std::sin (angle), centre.y - capRadius * 0.35f * std::cos (angle));
    g.setColour (enabled ? Theme::text : Theme::dim);
    g.drawLine ({ base, tip }, juce::jmax (2.0f, radius * 0.08f));
}

void GenieLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                             bool isHighlighted, bool isDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = backgroundColour;

    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.4f);
    else if (isDown)
        colour = colour.brighter (0.25f);
    else if (isHighlighted)
        colour = colour.brighter (0.12f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 5.0f);
}

void GenieLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool isHighlighted, bool)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const float h = juce::jmin (18.0f, bounds.getHeight());
    const juce::Rectangle<float> pill (bounds.getX(), bounds.getCentreY() - h * 0.5f, h * 1.8f, h);
    const bool on = button.getToggleState();

    g.setColour (on ? Theme::accent : Theme::panelEdge.brighter (isHighlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle (pill, h * 0.5f);

    const float d = h - 4.0f;
    g.setColour (on ? Theme::background : Theme::dim);
    g.fillEllipse (on ? pill.getRight() - d - 2.0f : pill.getX() + 2.0f, pill.getY() + 2.0f, d, d);

    g.setColour (button.isEnabled() ? Theme::text : Theme::dim);
    g.setFont (uiFont (13.0f));
    g.drawFittedText (button.getButtonText(), bounds.withTrimmedLeft (pill.getWidth() + 8.0f).toNearestInt(),
                      juce::Justification::centredLeft, 1);
}

juce::Font GenieLookAndFeel::getTextButtonFont (juce::TextButton&, int) { return uiFont (13.0f); }
juce::Font GenieLookAndFeel::getComboBoxFont (juce::ComboBox&) { return uiFont (13.0f); }
juce::Font GenieLookAndFeel::getLabelFont (juce::Label& l) { return l.getFont(); }
juce::Font GenieLookAndFeel::getPopupMenuFont() { return uiFont (14.0f); }

juce::Label* GenieLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);
    l->setFont (uiFont (12.0f));
    l->setJustificationType (juce::Justification::centred);
    return l;
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& title,
            const juce::String& suffix, int decimals)
{
    label.setText (title, juce::dontSendNotification);
    label.setFont (uiFont (12.0f, true));
    label.setColour (juce::Label::textColourId, Theme::dim);
    label.setJustificationType (juce::Justification::centred);
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);

    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 18);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxTextColourId, Theme::text);
    slider.setTitle (title);
    addAndMakeVisible (slider);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramID, slider);

    // Friendlier number formatting than the parameter's default.
    slider.textFromValueFunction = [suffix, decimals] (double v)
    {
        // (juce::String (v, 0) does not round to a whole number)
        return (decimals > 0 ? juce::String (v, decimals) : juce::String (juce::roundToInt (v)))
             + (suffix.isEmpty() ? juce::String() : " " + suffix);
    };
    slider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.-+").getDoubleValue(); };
    slider.updateText();
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

//==============================================================================
WaveformView::WaveformView()
{
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void WaveformView::setWaveform (const std::vector<float>& mins, const std::vector<float>& maxs, double durationSeconds)
{
    mn = mins;
    mx = maxs;
    duration = durationSeconds;
    repaint();
}

void WaveformView::setSelection (double startSec, double endSec)
{
    if (! dragging)
    {
        selStart = startSec;
        selEnd = endSec;
        repaint();
    }
}

void WaveformView::setUsedRange (double startSec, double endSec)
{
    usedStart = startSec;
    usedEnd = endSec;
    repaint();
}

void WaveformView::setProgress (bool isBusy, float progress01)
{
    if (busy != isBusy || std::abs (progress - progress01) > 0.002f)
    {
        busy = isBusy;
        progress = progress01;
        repaint();
    }
}

void WaveformView::setHint (const juce::String& text)
{
    if (hint != text)
    {
        hint = text;
        repaint();
    }
}

double WaveformView::xToTime (float x) const
{
    return duration <= 0.0 ? 0.0 : juce::jlimit (0.0, duration, (double) x / (double) juce::jmax (1, getWidth()) * duration);
}

float WaveformView::timeToX (double t) const
{
    return duration <= 0.0 ? 0.0f : (float) (t / duration * getWidth());
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::background);
    g.fillRoundedRectangle (bounds, 6.0f);

    const bool hasWave = ! mn.empty() && mn.size() == mx.size() && duration > 0.0;

    if (hasWave)
    {
        // what the analysis used
        if (usedEnd > usedStart)
        {
            g.setColour (Theme::accent.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (timeToX (usedStart), 0.0f, timeToX (usedEnd) - timeToX (usedStart), bounds.getHeight()));
        }

        const float mid = bounds.getCentreY();
        const float scale = bounds.getHeight() * 0.46f;
        const auto n = mn.size();

        float peak = 1.0e-4f;
        for (size_t i = 0; i < n; ++i)
            peak = juce::jmax (peak, -mn[i], mx[i]);
        const float norm = 1.0f / peak;

        juce::Path p;
        for (size_t i = 0; i < n; ++i)
        {
            const float x = bounds.getWidth() * (float) i / (float) (n - 1);
            const float y = mid - mx[i] * norm * scale;
            if (i == 0)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);
        }
        for (size_t i = n; i-- > 0;)
            p.lineTo (bounds.getWidth() * (float) i / (float) (n - 1), mid - mn[i] * norm * scale);
        p.closeSubPath();

        g.setColour (Theme::dim.withAlpha (0.75f));
        g.fillPath (p);

        if (usedEnd > usedStart)
        {
            g.saveState();
            g.reduceClipRegion (juce::Rectangle<float> (timeToX (usedStart), 0.0f, timeToX (usedEnd) - timeToX (usedStart), bounds.getHeight()).toNearestInt());
            g.setColour (Theme::accent);
            g.fillPath (p);
            g.restoreState();
        }

        // the user's selection
        if (selEnd > selStart)
        {
            const juce::Rectangle<float> sel (timeToX (selStart), 0.0f, timeToX (selEnd) - timeToX (selStart), bounds.getHeight());
            g.setColour (Theme::live.withAlpha (0.16f));
            g.fillRect (sel);
            g.setColour (Theme::live);
            g.fillRect (sel.withWidth (1.5f));
            g.fillRect (sel.withLeft (sel.getRight() - 1.5f));
        }

        g.setColour (Theme::dim);
        g.setFont (uiFont (11.0f));
        const int total = (int) duration;
        g.drawText (juce::String::formatted ("%d:%02d", total / 60, total % 60), getLocalBounds().reduced (6, 3), juce::Justification::bottomRight);
        g.drawText (selEnd > selStart ? "Double-click to clear the selection" : "Drag to choose the part with the tone you want",
                    getLocalBounds().reduced (6, 3), juce::Justification::bottomLeft);
    }
    else
    {
        g.setColour (Theme::dim);
        g.setFont (uiFont (15.0f));
        g.drawFittedText (hint, getLocalBounds().reduced (20), juce::Justification::centred, 3);
    }

    if (busy)
    {
        g.setColour (Theme::background.withAlpha (0.55f));
        g.fillRoundedRectangle (bounds, 6.0f);

        auto bar = bounds.withSizeKeepingCentre (juce::jmin (320.0f, bounds.getWidth() - 40.0f), 6.0f);
        g.setColour (Theme::panelEdge);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (6.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress))), 3.0f);
    }

    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (duration <= 0.0 || busy)
        return;

    dragging = true;
    dragAnchor = xToTime ((float) e.x);
    selStart = selEnd = dragAnchor;
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    const double t = xToTime ((float) e.x);
    selStart = juce::jmin (dragAnchor, t);
    selEnd = juce::jmax (dragAnchor, t);
    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;

    if (selEnd - selStart < 1.0)
    {
        // too short to be a region: treat as a stray click and keep things as they were
        selStart = selEnd = -1.0;
        repaint();
        return;
    }

    if (onSelectionChanged)
        onSelectionChanged (selStart, selEnd);
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (duration <= 0.0 || busy)
        return;

    dragging = false;
    selStart = selEnd = -1.0;
    repaint();

    if (onSelectionChanged)
        onSelectionChanged (-1.0, -1.0);
}

//==============================================================================
void SpectrumView::setData (const tg::BandArray& t, const tg::BandArray& o, const tg::BandArray& e, bool targetValid, bool isPlaying)
{
    target = t;
    output = o;
    eq = e;
    hasTarget = targetValid;
    playing = isPlaying;
    repaint();
}

juce::Path SpectrumView::makePath (const tg::BandArray& db, juce::Rectangle<float> area, float dbTop, float dbBottom, bool closed) const
{
    juce::Path p;
    const int first = 3, last = tg::kNumBands - 4; // ~57 Hz .. ~11.5 kHz

    for (int b = first; b <= last; ++b)
    {
        const float x = area.getX() + area.getWidth() * (float) (b - first) / (float) (last - first);
        const float v = juce::jlimit (dbBottom, dbTop, db[(size_t) b]);
        const float y = area.getY() + area.getHeight() * (dbTop - v) / (dbTop - dbBottom);
        if (b == first)
            p.startNewSubPath (x, y);
        else
            p.lineTo (x, y);
    }

    if (closed)
    {
        p.lineTo (area.getRight(), area.getBottom());
        p.lineTo (area.getX(), area.getBottom());
        p.closeSubPath();
    }
    return p;
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::background);
    g.fillRoundedRectangle (bounds, 6.0f);

    const auto area = bounds.reduced (8.0f, 6.0f).withTrimmedBottom (14.0f);
    const float dbTop = 18.0f, dbBottom = -42.0f;
    const int first = 3, last = tg::kNumBands - 4;

    // frequency grid
    g.setFont (uiFont (10.0f));
    for (float hz : { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float pos = (tg::bandIndexForHz (hz) - (float) first) / (float) (last - first);
        if (pos < 0.0f || pos > 1.0f)
            continue;
        const float x = area.getX() + area.getWidth() * pos;
        g.setColour (Theme::panelEdge);
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
        g.setColour (Theme::dim);
        g.drawText (hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String (juce::roundToInt (hz)),
                    juce::Rectangle<float> (x - 20.0f, area.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
    }

    g.saveState();
    g.reduceClipRegion (area.toNearestInt());

    if (hasTarget)
    {
        g.setColour (Theme::accent.withAlpha (0.22f));
        g.fillPath (makePath (target, area, dbTop, dbBottom, true));
        g.setColour (Theme::accent);
        g.strokePath (makePath (target, area, dbTop, dbBottom, false), juce::PathStrokeType (1.6f));
    }

    g.setColour (Theme::dim.withAlpha (0.8f));
    const float dashes[] = { 4.0f, 3.0f };
    juce::Path eqPath, dashed;
    eqPath = makePath (eq, area, dbTop, dbBottom, false);
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, eqPath, dashes, 2);
    g.fillPath (dashed);

    g.setColour (playing ? Theme::live : Theme::live.withAlpha (0.45f));
    g.strokePath (makePath (output, area, dbTop, dbBottom, false), juce::PathStrokeType (1.6f));
    g.restoreState();

    // legend
    g.setFont (uiFont (11.0f));
    auto legend = bounds.reduced (12.0f, 8.0f).removeFromTop (14.0f);
    const auto item = [&] (juce::Colour c, const juce::String& text, float width)
    {
        auto r = legend.removeFromRight (width);
        g.setColour (c);
        g.fillRect (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 2.0f));
        g.setColour (Theme::dim);
        g.drawText (text, r.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
    };
    item (Theme::dim, "EQ applied", 78.0f);
    item (Theme::live, "Your output", 86.0f);
    if (hasTarget)
        item (Theme::accent, "Reference tone", 100.0f);

    g.setColour (Theme::panelEdge);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);
}

//==============================================================================
void LevelMeter::setLevel (float db)
{
    if (std::abs (db - level) > 0.2f)
    {
        level = db;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::panelEdge);
    g.fillRoundedRectangle (bounds, 2.0f);

    const float frac = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
    g.setColour (level > -1.0f ? Theme::warn : Theme::live);
    g.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * frac), 2.0f);
}
