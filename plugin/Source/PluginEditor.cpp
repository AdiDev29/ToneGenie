#include "PluginEditor.h"

ToneGenieEditor::ToneGenieEditor (ToneGenieProcessor& p)
    : juce::AudioProcessorEditor (&p),
      genie (p),
      drive (p.apvts, ParamIDs::drive, "DRIVE", "%", 0),
      comp (p.apvts, ParamIDs::comp, "SUSTAIN", "%", 0),
      gate (p.apvts, ParamIDs::gate, "GATE", "%", 0),
      match (p.apvts, ParamIDs::match, "TONE MATCH", "%", 0),
      reverb (p.apvts, ParamIDs::reverbMix, "REVERB", "%", 0),
      decay (p.apvts, ParamIDs::reverbDecay, "DECAY", "s", 1),
      delay (p.apvts, ParamIDs::delayMix, "DELAY", "%", 0),
      delayTime (p.apvts, ParamIDs::delayTime, "TIME", "ms", 0),
      feedback (p.apvts, ParamIDs::delayFeedback, "REPEATS", "%", 0),
      mix (p.apvts, ParamIDs::mix, "MIX", "%", 0),
      output (p.apvts, ParamIDs::output, "OUTPUT", "dB", 1)
{
    setLookAndFeel (&lookAndFeel);

    // --- reference section -----------------------------------------------------
    addAndMakeVisible (loadButton);
    loadButton.setColour (juce::TextButton::buttonColourId, Theme::accent);
    loadButton.setColour (juce::TextButton::textColourOffId, Theme::background);
    loadButton.onClick = [this] { chooseFile(); };

    addAndMakeVisible (auditionButton);
    auditionButton.setTooltip ("Listen to the guitar signal the tone was taken from");
    auditionButton.onClick = [this] { genie.setAuditioning (! genie.isAuditioning()); };

    addAndMakeVisible (clearButton);
    clearButton.onClick = [this]
    {
        currentFile = juce::File();
        regionStart = regionEnd = -1.0;
        genie.clearReference();
    };

    fileLabel.setFont (uiFont (13.0f, true));
    fileLabel.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (fileLabel);

    statusLabel.setFont (uiFont (13.0f));
    statusLabel.setColour (juce::Label::textColourId, Theme::text);
    statusLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (statusLabel);

    const auto setupSmallLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (uiFont (12.0f, true));
        l.setColour (juce::Label::textColourId, Theme::dim);
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };

    setupSmallLabel (sourceLabel, "FILE IS");
    sourceBox.addItemList ({ "Auto-detect", "Guitar only", "Full song" }, 1);
    sourceBox.setSelectedId (1, juce::dontSendNotification);
    sourceBox.setTooltip ("Whether the file is just a guitar or a whole mix that the guitar has to be isolated from");
    sourceBox.onChange = [this] { if (! updatingBoxes) reanalyse(); };
    addAndMakeVisible (sourceBox);

    setupSmallLabel (focusLabel, "GUITAR SITS");
    focusBox.addItemList ({ "Auto", "Left", "Centre", "Right" }, 1);
    focusBox.setSelectedId (1, juce::dontSendNotification);
    focusBox.setTooltip ("Where the guitar you want is panned in the mix");
    focusBox.onChange = [this] { if (! updatingBoxes) reanalyse(); };
    addAndMakeVisible (focusBox);

    waveform.onSelectionChanged = [this] (double start, double end)
    {
        regionStart = start;
        regionEnd = end;
        reanalyse();
    };
    addAndMakeVisible (waveform);

    // --- tone display -------------------------------------------------------------
    addAndMakeVisible (spectrum);

    // --- controls -------------------------------------------------------------------
    voicingLabel.setText ("VOICING", juce::dontSendNotification);
    voicingLabel.setFont (uiFont (12.0f, true));
    voicingLabel.setColour (juce::Label::textColourId, Theme::dim);
    voicingLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (voicingLabel);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (genie.apvts.getParameter (ParamIDs::voicing)))
        voicingBox.addItemList (choice->choices, 1);
    addAndMakeVisible (voicingBox);
    voicingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (genie.apvts, ParamIDs::voicing, voicingBox);

    for (auto* k : { &drive, &comp, &gate, &match, &reverb, &decay, &delay, &delayTime, &feedback, &mix, &output })
        addAndMakeVisible (*k);

    adaptButton.setTooltip ("Keep adjusting to the guitar you are playing so the output tone stays on target. "
                            "Switch off to freeze the current correction.");
    addAndMakeVisible (adaptButton);
    adaptAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (genie.apvts, ParamIDs::adapt, adaptButton);

    addAndMakeVisible (bypassButton);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (genie.apvts, ParamIDs::bypass, bypassButton);

    relearnButton.setTooltip ("Changed guitars or pickups? Forget what was learned and start again.");
    relearnButton.onClick = [this] { genie.relearnInstrument(); };
    addAndMakeVisible (relearnButton);

    trimLabel.setFont (uiFont (12.0f));
    trimLabel.setColour (juce::Label::textColourId, Theme::dim);
    trimLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (trimLabel);

    addAndMakeVisible (meter);

    // Children created before the look-and-feel was installed need to pick it up.
    sendLookAndFeelChange();

    setResizable (true, true);
    setResizeLimits (820, 660, 1600, 1100);
    setSize (940, 740);

    refreshFromReference();
    startTimerHz (30);
}

ToneGenieEditor::~ToneGenieEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void ToneGenieEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    // header
    auto header = getLocalBounds().removeFromTop (48).reduced (16, 0);
    g.setColour (Theme::accent);
    g.setFont (uiFont (24.0f, true));
    g.drawText ("ToneGenie", header.removeFromLeft (150), juce::Justification::centredLeft);
    g.setColour (Theme::dim);
    g.setFont (uiFont (13.0f));
    g.drawText ("Play any guitar. Get that tone.", header.removeFromLeft (260), juce::Justification::centredLeft);

    for (const auto& area : { referenceArea, toneArea, controlArea })
    {
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (area.toFloat(), 8.0f);
    }

    const auto title = [&] (juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour (Theme::dim);
        g.setFont (uiFont (11.0f, true));
        g.drawText (text, area.reduced (12, 6).removeFromTop (12), juce::Justification::topLeft);
    };

    title (referenceArea, "1  REFERENCE - THE SOUND YOU WANT");
    title (toneArea, "2  TONE - REFERENCE VS. WHAT YOU ARE PLAYING");
    title (controlArea, "3  PEDAL - SET AUTOMATICALLY, TWEAK TO TASTE");
}

void ToneGenieEditor::paintOverChildren (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (Theme::accent.withAlpha (0.12f));
        g.fillRoundedRectangle (referenceArea.toFloat(), 8.0f);
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (referenceArea.toFloat().reduced (1.0f), 8.0f, 2.0f);
    }
}

void ToneGenieEditor::resized()
{
    auto r = getLocalBounds();

    // header
    {
        auto header = r.removeFromTop (48).reduced (16, 0);
        bypassButton.setBounds (header.removeFromRight (96).withSizeKeepingCentre (96, 24));
        header.removeFromRight (12);
        meter.setBounds (header.removeFromRight (120).withSizeKeepingCentre (120, 6));
    }

    r.reduce (12, 0);
    r.removeFromBottom (12);

    const int controlsHeight = 258;
    const int flexible = r.getHeight() - controlsHeight - 2 * 10;
    referenceArea = r.removeFromTop (juce::roundToInt (flexible * 0.56f));
    r.removeFromTop (10);
    toneArea = r.removeFromTop (flexible - referenceArea.getHeight());
    r.removeFromTop (10);
    controlArea = r;

    // --- reference ----------------------------------------------------------------
    {
        auto a = referenceArea.reduced (12, 0);
        a.removeFromTop (24);
        auto row = a.removeFromTop (28);

        loadButton.setBounds (row.removeFromLeft (112));
        row.removeFromLeft (10);

        clearButton.setBounds (row.removeFromRight (58));
        row.removeFromRight (6);
        auditionButton.setBounds (row.removeFromRight (84));
        row.removeFromRight (14);
        focusBox.setBounds (row.removeFromRight (86));
        focusLabel.setBounds (row.removeFromRight (86).withTrimmedRight (6));
        sourceBox.setBounds (row.removeFromRight (112));
        sourceLabel.setBounds (row.removeFromRight (56).withTrimmedRight (6));
        fileLabel.setBounds (row);

        a.removeFromBottom (8);
        statusLabel.setBounds (a.removeFromBottom (22));
        a.removeFromTop (8);
        a.removeFromBottom (4);
        waveform.setBounds (a);
    }

    // --- tone ----------------------------------------------------------------------
    {
        auto a = toneArea.reduced (12, 0);
        a.removeFromTop (24);
        a.removeFromBottom (10);
        spectrum.setBounds (a);
    }

    // --- controls --------------------------------------------------------------------
    {
        auto a = controlArea.reduced (12, 0);
        a.removeFromTop (26);
        a.removeFromBottom (8);

        auto top = a.removeFromTop ((a.getHeight() - 12) / 2);
        a.removeFromTop (12);
        auto bottom = a;

        // right-hand column: instrument adaptation
        auto adaptColumn = top.removeFromRight (190);
        bottom.removeFromRight (190);
        adaptColumn = adaptColumn.withBottom (bottom.getBottom()).reduced (8, 2);
        adaptButton.setBounds (adaptColumn.removeFromTop (24));
        adaptColumn.removeFromTop (8);
        relearnButton.setBounds (adaptColumn.removeFromTop (26).withWidth (130));
        adaptColumn.removeFromTop (10);
        trimLabel.setBounds (adaptColumn);

        const auto layoutRow = [] (juce::Rectangle<int> row, std::initializer_list<juce::Component*> items)
        {
            const int w = row.getWidth() / (int) items.size();
            for (auto* c : items)
                c->setBounds (row.removeFromLeft (w).reduced (3, 0));
        };

        // voicing selector takes the first cell of the top row
        const int cell = top.getWidth() / 5;
        auto voiceCell = top.removeFromLeft (cell).reduced (8, 0);
        voicingLabel.setBounds (voiceCell.removeFromTop (16));
        voicingBox.setBounds (voiceCell.withSizeKeepingCentre (voiceCell.getWidth(), 28));

        layoutRow (top, { &drive, &comp, &gate, &match });
        layoutRow (bottom, { &reverb, &decay, &delay, &delayTime, &feedback, &mix, &output });
    }
}

//==============================================================================
bool ToneGenieEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && ReferenceManager::isSupportedFile (juce::File (files[0]));
}

void ToneGenieEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint();
}

void ToneGenieEditor::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint();
}

void ToneGenieEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();

    if (files.size() == 1)
    {
        regionStart = regionEnd = -1.0;
        analyse (juce::File (files[0]));
    }
}

void ToneGenieEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a recording with the guitar tone you want",
                                                   currentFile.existsAsFile() ? currentFile.getParentDirectory() : juce::File(),
                                                   ReferenceManager::getFileWildcard());

    juce::Component::SafePointer<ToneGenieEditor> safe (this);

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                              {
                                  safe->regionStart = safe->regionEnd = -1.0;
                                  safe->analyse (file);
                              }
                          });
}

ReferenceSettings ToneGenieEditor::currentSettings() const
{
    ReferenceSettings s;
    s.sourceMode = juce::jmax (0, sourceBox.getSelectedId() - 1);
    s.focus = juce::jmax (0, focusBox.getSelectedId() - 1);
    s.regionStart = regionStart;
    s.regionEnd = regionEnd;
    return s;
}

void ToneGenieEditor::analyse (const juce::File& file)
{
    currentFile = file;
    genie.loadReference (file, currentSettings());
}

void ToneGenieEditor::reanalyse()
{
    if (currentFile.existsAsFile())
        analyse (currentFile);
    else if (currentFile != juce::File())
        statusLabel.setText ("The reference file is no longer at " + currentFile.getFullPathName() + " - load it again to re-analyse.",
                             juce::dontSendNotification);
}

void ToneGenieEditor::refreshFromReference()
{
    const auto snap = genie.reference.getSnapshot();

    currentFile = snap.filePath.isNotEmpty() ? juce::File (snap.filePath) : juce::File();
    regionStart = snap.settings.regionStart;
    regionEnd = snap.settings.regionEnd;

    updatingBoxes = true;
    sourceBox.setSelectedId (snap.settings.sourceMode + 1, juce::dontSendNotification);
    focusBox.setSelectedId (snap.settings.focus + 1, juce::dontSendNotification);
    updatingBoxes = false;

    fileLabel.setText (currentFile != juce::File() ? currentFile.getFileName() : juce::String ("No reference loaded"), juce::dontSendNotification);
    fileLabel.setColour (juce::Label::textColourId, currentFile != juce::File() ? Theme::text : Theme::dim);
    statusLabel.setText (snap.status, juce::dontSendNotification);

    waveform.setWaveform (snap.thumbMin, snap.thumbMax, snap.duration);
    waveform.setSelection (regionStart, regionEnd);
    waveform.setHint (snap.status);

    if (snap.hasProfile)
        waveform.setUsedRange (snap.profile.usedStartSec, snap.profile.usedEndSec);
    else
        waveform.setUsedRange (-1.0, -1.0);
}

void ToneGenieEditor::timerCallback()
{
    const int revision = genie.reference.getRevision();
    if (revision != lastRevision)
    {
        lastRevision = revision;
        refreshFromReference();
    }

    const bool busy = genie.reference.isBusy();
    waveform.setProgress (busy, genie.reference.getProgress());

    const auto snap = genie.reference.getSnapshot();
    const bool hasProfile = snap.hasProfile;
    const bool wantFocus = sourceBox.getSelectedId() != 2;

    focusBox.setEnabled (wantFocus && ! busy);
    sourceBox.setEnabled (! busy);
    loadButton.setEnabled (! busy);
    clearButton.setEnabled (! busy && (hasProfile || currentFile != juce::File()));
    auditionButton.setEnabled (genie.hasAudition() && ! busy);
    auditionButton.setButtonText (genie.isAuditioning() ? "Stop" : "Audition");
    auditionButton.setToggleState (genie.isAuditioning(), juce::dontSendNotification);

    tg::BandArray target, out, eq;
    genie.getSpectrum (target, out, eq);
    spectrum.setData (target, out, eq, hasProfile, genie.isPlaying());

    const bool adapting = adaptButton.getToggleState();
    trimLabel.setText (juce::String (adapting ? "Listening to your guitar" : "Correction frozen")
                           + "\nInput trim: " + juce::String (genie.getInputTrimDb(), 1) + " dB"
                           + "\nPickup compensation: " + juce::String (genie.getPickupCorrectionDb(), 1) + " dB",
                       juce::dontSendNotification);

    meter.setLevel (genie.getOutputLevelDb());
}
