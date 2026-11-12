#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <tonegenie/Bands.h>

namespace Theme
{
    const juce::Colour background { 0xff14161a };
    const juce::Colour panel { 0xff1d2026 };
    const juce::Colour panelEdge { 0xff2b2f38 };
    const juce::Colour text { 0xffe9e6df };
    const juce::Colour dim { 0xff8b919c };
    const juce::Colour accent { 0xfff0a030 };
    const juce::Colour accentDark { 0xff8a5a14 };
    const juce::Colour live { 0xff58d0c0 };
    const juce::Colour warn { 0xffe0605a };
}

juce::Font uiFont (float height, bool bold = false);

//==============================================================================
class GenieLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GenieLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
/** A labelled rotary control bound to a parameter. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::String& title,
          const juce::String& suffix, int decimals);

    void resized() override;

private:
    juce::Label label;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
/** Overview of the reference file with a selectable region. */
class WaveformView : public juce::Component
{
public:
    WaveformView();

    void setWaveform (const std::vector<float>& mins, const std::vector<float>& maxs, double durationSeconds);
    void setSelection (double startSec, double endSec);   ///< negative = none
    void setUsedRange (double startSec, double endSec);   ///< what the analysis actually used
    void setProgress (bool busy, float progress01);
    void setHint (const juce::String& text);

    /** Called when the user finishes dragging a region (or clears it: both negative). */
    std::function<void (double startSec, double endSec)> onSelectionChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    double xToTime (float x) const;
    float timeToX (double t) const;

    std::vector<float> mn, mx;
    double duration = 0.0;
    double selStart = -1.0, selEnd = -1.0, usedStart = -1.0, usedEnd = -1.0;
    double dragAnchor = 0.0;
    bool dragging = false, busy = false;
    float progress = 0.0f;
    juce::String hint;
};

//==============================================================================
/** Target tone vs. what is coming out right now, and the EQ in between. */
class SpectrumView : public juce::Component
{
public:
    void setData (const tg::BandArray& target, const tg::BandArray& output, const tg::BandArray& eq, bool hasTarget, bool playing);
    void paint (juce::Graphics&) override;

private:
    juce::Path makePath (const tg::BandArray& db, juce::Rectangle<float> area, float dbTop, float dbBottom, bool closed) const;

    tg::BandArray target {}, output {}, eq {};
    bool hasTarget = false, playing = false;
};

//==============================================================================
class LevelMeter : public juce::Component
{
public:
    void setLevel (float db);
    void paint (juce::Graphics&) override;

private:
    float level = -100.0f;
};
