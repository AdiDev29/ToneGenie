#pragma once

#include "PluginProcessor.h"
#include "Widgets.h"

class ToneGenieEditor : public juce::AudioProcessorEditor,
                        public juce::FileDragAndDropTarget,
                        private juce::Timer
{
public:
    explicit ToneGenieEditor (ToneGenieProcessor&);
    ~ToneGenieEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void refreshFromReference();
    void chooseFile();
    void analyse (const juce::File& file);
    void reanalyse();
    ReferenceSettings currentSettings() const;

    ToneGenieProcessor& genie;
    GenieLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };

    // reference section
    juce::TextButton loadButton { "Load audio..." }, auditionButton { "Audition" }, clearButton { "Clear" };
    juce::Label fileLabel, statusLabel, sourceLabel, focusLabel;
    juce::ComboBox sourceBox, focusBox;
    WaveformView waveform;

    // tone display
    SpectrumView spectrum;

    // controls
    juce::Label voicingLabel;
    juce::ComboBox voicingBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> voicingAttachment;
    Knob drive, comp, gate, match, reverb, decay, delay, delayTime, feedback, mix, output;

    juce::ToggleButton adaptButton { "Live adapt" }, bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> adaptAttachment, bypassAttachment;
    juce::TextButton relearnButton { "Re-learn guitar" };
    juce::Label trimLabel;
    LevelMeter meter;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::File currentFile;
    double regionStart = -1.0, regionEnd = -1.0;
    int lastRevision = -1;
    bool dragOver = false, updatingBoxes = false;

    juce::Rectangle<int> referenceArea, toneArea, controlArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToneGenieEditor)
};
