#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>

namespace
{
    juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    std::unique_ptr<juce::AudioParameterFloat> percentParam (const char* paramID, const char* name, float defaultValue)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { paramID, 1 }, name, juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), defaultValue,
            juce::AudioParameterFloatAttributes().withLabel ("%"));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ToneGenieProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (percentParam (ParamIDs::drive, "Drive", 50.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::voicing, 1 }, "Voicing",
                                                        StringArray { "Smooth", "Crunch", "Fuzz" }, 1));
    layout.add (percentParam (ParamIDs::comp, "Compression", 0.0f));
    layout.add (percentParam (ParamIDs::gate, "Gate", 20.0f));
    layout.add (percentParam (ParamIDs::match, "Tone Match", 100.0f));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::adapt, 1 }, "Live Adapt", true));
    layout.add (percentParam (ParamIDs::reverbMix, "Reverb", 5.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::reverbDecay, 1 }, "Reverb Decay",
                                                       skewedRange (0.2f, 6.0f, 1.5f), 1.2f,
                                                       AudioParameterFloatAttributes().withLabel ("s")));
    layout.add (percentParam (ParamIDs::delayMix, "Delay", 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::delayTime, 1 }, "Delay Time",
                                                       skewedRange (20.0f, 1200.0f, 350.0f), 380.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::delayFeedback, 1 }, "Delay Feedback",
                                                       NormalisableRange<float> (0.0f, 90.0f, 0.1f), 30.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (percentParam (ParamIDs::mix, "Mix", 100.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::output, 1 }, "Output",
                                                       NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::bypass, 1 }, "Bypass", false));
    return layout;
}

ToneGenieProcessor::ToneGenieProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
    pDrive = apvts.getRawParameterValue (ParamIDs::drive);
    pVoicing = apvts.getRawParameterValue (ParamIDs::voicing);
    pComp = apvts.getRawParameterValue (ParamIDs::comp);
    pGate = apvts.getRawParameterValue (ParamIDs::gate);
    pMatch = apvts.getRawParameterValue (ParamIDs::match);
    pAdapt = apvts.getRawParameterValue (ParamIDs::adapt);
    pReverbMix = apvts.getRawParameterValue (ParamIDs::reverbMix);
    pReverbDecay = apvts.getRawParameterValue (ParamIDs::reverbDecay);
    pDelayMix = apvts.getRawParameterValue (ParamIDs::delayMix);
    pDelayTime = apvts.getRawParameterValue (ParamIDs::delayTime);
    pDelayFeedback = apvts.getRawParameterValue (ParamIDs::delayFeedback);
    pMix = apvts.getRawParameterValue (ParamIDs::mix);
    pOutput = apvts.getRawParameterValue (ParamIDs::output);
    pBypass = apvts.getRawParameterValue (ParamIDs::bypass);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::bypass));

    setLatencySamples (tg::ToneEngine::latencySamples());
}

ToneGenieProcessor::~ToneGenieProcessor()
{
    reference.shutdown(); // no analysis callback may arrive once members start going away
    designThread.stopThread (4000);
}

//==============================================================================
bool ToneGenieProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    // mono -> mono, mono -> stereo, stereo -> stereo
    return in == juce::AudioChannelSet::mono() || (in == juce::AudioChannelSet::stereo() && out == in);
}

void ToneGenieProcessor::prepareToPlay (double sampleRate, int)
{
    designThread.stopThread (4000);

    currentSampleRate = sampleRate;

    // A state restored before audio started is applied now, so that the very
    // first block (and an offline render of a fresh instance) already has it.
    {
        const juce::SpinLock::ScopedLockType sl (pendingLock);
        if (pendingFlag.load (std::memory_order_acquire))
        {
            engine.setProfile (pendingHasProfile ? &pendingProfile : nullptr);
            if (pendingHasAdapt)
                engine.setAdaptState (pendingAdapt);
            pendingHasAdapt = false;
            pendingFlag.store (false, std::memory_order_release);
        }
    }

    engine.setParams (readParams (pBypass->load() > 0.5f));
    engine.prepare (sampleRate);
    engine.reset();
    setLatencySamples (tg::ToneEngine::latencySamples());

    designThread.startThread (juce::Thread::Priority::low);
}

void ToneGenieProcessor::releaseResources()
{
    designThread.stopThread (4000);
    auditionPlaying.store (false);
}

tg::EngineParams ToneGenieProcessor::readParams (bool bypassed) const
{
    tg::EngineParams p;
    p.drive = pDrive->load() * 0.01f;
    p.voicing = juce::roundToInt (pVoicing->load());
    p.comp = pComp->load() * 0.01f;
    p.gate = pGate->load() * 0.01f;
    p.match = pMatch->load() * 0.01f;
    p.adapt = pAdapt->load() > 0.5f;
    p.reverbMix = pReverbMix->load() * 0.01f;
    p.reverbDecay = pReverbDecay->load();
    p.delayMix = pDelayMix->load() * 0.01f;
    p.delayTimeMs = pDelayTime->load();
    p.delayFeedback = pDelayFeedback->load() * 0.01f;
    p.mix = pMix->load() * 0.01f;
    p.outputDb = pOutput->load();

    if (bypassed)
    {
        // Keep running (and keep the latency constant) but pass the dry signal.
        p.mix = 0.0f;
        p.outputDb = 0.0f;
        p.adapt = false;
    }

    return p;
}

void ToneGenieProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, pBypass->load() > 0.5f);
}

void ToneGenieProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, true);
}

void ToneGenieProcessor::process (juce::AudioBuffer<float>& buffer, bool bypassed)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    if (numSamples == 0 || numOut == 0 || buffer.getNumChannels() == 0)
        return;

    // --- take over anything the other threads prepared ---------------------------
    if (pendingFlag.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType tryLock (pendingLock);
        if (tryLock.isLocked())
        {
            engine.setProfile (pendingHasProfile ? &pendingProfile : nullptr);
            if (pendingHasAdapt)
                engine.setAdaptState (pendingAdapt);
            pendingHasAdapt = false;
            pendingFlag.store (false, std::memory_order_release);
        }
    }

    if (relearnRequested.exchange (false))
        engine.relearn();

    const bool offline = isNonRealtime();
    designThread.offline.store (offline, std::memory_order_relaxed);
    engine.setParams (readParams (bypassed));
    engine.setSynchronous (offline);

    // --- the effect -----------------------------------------------------------------
    const float* inL = numIn > 0 ? buffer.getReadPointer (0) : nullptr;
    const float* inR = numIn > 1 && buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;
    float* outL = buffer.getWritePointer (0);
    float* outR = numOut > 1 && buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    if (inL == nullptr)
    {
        buffer.clear();
        inL = buffer.getReadPointer (0);
    }

    engine.process (inL, inR, outL, outR, numSamples);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // --- audition the guitar the tone was learned from --------------------------------
    if (auditionPlaying.load (std::memory_order_relaxed))
    {
        const juce::SpinLock::ScopedTryLockType tryLock (auditionLock);
        if (tryLock.isLocked())
        {
            if (auditionRewind.exchange (false))
                auditionPos = 0.0;

            const auto size = (double) auditionData.size();
            const double step = auditionRate / currentSampleRate;

            for (int i = 0; i < numSamples; ++i)
            {
                float v = 0.0f;
                if (auditionPos + 1.0 < size)
                {
                    const auto idx = (size_t) auditionPos;
                    const auto frac = (float) (auditionPos - (double) idx);
                    v = auditionData[idx] + frac * (auditionData[idx + 1] - auditionData[idx]);
                    auditionPos += step;
                }
                else
                {
                    auditionPlaying.store (false, std::memory_order_relaxed);
                }

                outL[i] = v;
                if (outR != nullptr)
                    outR[i] = v;
            }
        }
    }

    // --- metering -------------------------------------------------------------------------
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        peak = juce::jmax (peak, std::abs (outL[i]));

    const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
    const float previous = outputLevelDb.load (std::memory_order_relaxed);
    const float decay = 30.0f * (float) numSamples / (float) currentSampleRate; // 30 dB/s fall
    outputLevelDb.store (juce::jmax (peakDb, previous - decay), std::memory_order_relaxed);
}

//==============================================================================
void ToneGenieProcessor::setParam (const char* paramID, float plainValue)
{
    if (auto* p = apvts.getParameter (paramID))
    {
        const float normalised = p->convertTo0to1 (plainValue);
        p->beginChangeGesture();
        p->setValueNotifyingHost (normalised);
        p->endChangeGesture();
    }
}

void ToneGenieProcessor::pushProfile (const tg::ToneProfile* profile, const tg::AdaptState* adaptState)
{
    const juce::SpinLock::ScopedLockType sl (pendingLock);

    pendingHasProfile = profile != nullptr && profile->valid;
    if (pendingHasProfile)
        pendingProfile = *profile;

    pendingHasAdapt = adaptState != nullptr && adaptState->valid;
    if (pendingHasAdapt)
        pendingAdapt = *adaptState;

    pendingFlag.store (true, std::memory_order_release);
}

void ToneGenieProcessor::loadReference (const juce::File& file, const ReferenceSettings& settings)
{
    setAuditioning (false);
    reference.analyse (file, settings);
}

void ToneGenieProcessor::clearReference()
{
    setAuditioning (false);
    auditionAvailable.store (false);
    reference.clear();
    pushProfile (nullptr, nullptr);

    std::vector<float> old;
    {
        const juce::SpinLock::ScopedLockType sl (auditionLock);
        old.swap (auditionData);
    }
}

void ToneGenieProcessor::referenceAnalysed (const tg::ToneProfile& profile, std::vector<float>&& guitar, double guitarSampleRate)
{
    // Set the pedal the way the analysis says...
    setParam (ParamIDs::voicing, (float) profile.voicing);
    setParam (ParamIDs::drive, profile.drive * 100.0f);
    setParam (ParamIDs::comp, profile.comp * 100.0f);
    setParam (ParamIDs::gate, profile.gate * 100.0f);
    setParam (ParamIDs::reverbMix, profile.reverbMix * 100.0f);
    setParam (ParamIDs::reverbDecay, profile.reverbDecay);
    setParam (ParamIDs::match, 100.0f);

    // ...and hand the spectral target to the audio thread.
    pushProfile (&profile, nullptr);

    // Keep the guitar signal so the user can listen to what was analysed.
    // Normalise it to a comfortable level first.
    float peak = 0.0f;
    for (float v : guitar)
        peak = juce::jmax (peak, std::abs (v));
    if (peak > 1.0e-6f)
    {
        const float g = 0.5f / peak;
        for (auto& v : guitar)
            v *= g;
    }

    auditionPlaying.store (false);
    {
        const juce::SpinLock::ScopedLockType sl (auditionLock);
        auditionData.swap (guitar);
        auditionRate = guitarSampleRate;
    }
    auditionAvailable.store (true);
    // (the previous buffer is released here, on the message thread, when `guitar` goes out of scope)
}

void ToneGenieProcessor::setAuditioning (bool shouldPlay)
{
    if (shouldPlay && auditionAvailable.load())
    {
        auditionRewind.store (true);
        auditionPlaying.store (true);
    }
    else
    {
        auditionPlaying.store (false);
    }
}

//==============================================================================
void ToneGenieProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("ToneGenie");
    root.setProperty ("version", 1, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (reference.toValueTree(), nullptr);

    // What the engine has learned about the instrument. If a restored state has
    // not reached the audio thread yet, that one is still the truth.
    tg::AdaptState adapt;
    {
        const juce::SpinLock::ScopedLockType sl (pendingLock);
        if (! pendingFlag.load (std::memory_order_acquire))
            adapt = engine.getAdaptState();
        else if (pendingHasAdapt)
            adapt = pendingAdapt;
    }

    if (auto xmlOnly = adapt.valid ? nullptr : root.createXml())
    {
        copyXmlToBinary (*xmlOnly, destData);
        return;
    }

    juce::ValueTree at ("Instrument");
    at.setProperty ("level", adapt.levelDb, nullptr);
    at.setProperty ("levelSeconds", adapt.levelSeconds, nullptr);
    at.setProperty ("tiltLow", adapt.tiltLow, nullptr);
    at.setProperty ("tiltHigh", adapt.tiltHigh, nullptr);
    at.setProperty ("tiltSeconds", adapt.tiltSeconds, nullptr);
    at.setProperty ("weight", adapt.measuredWeight, nullptr);
    at.setProperty ("bands", tg::kNumBands, nullptr);
    at.setProperty ("measured", juce::MemoryBlock (adapt.measuredDb.data(), adapt.measuredDb.size() * sizeof (float)).toBase64Encoding(), nullptr);
    root.appendChild (at, nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void ToneGenieProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    const auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("ToneGenie"))
        return;

    const auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid())
        apvts.replaceState (params);

    tg::ToneProfile profile;
    const bool hasProfile = reference.fromValueTree (root.getChildWithName ("Reference"), profile);

    tg::AdaptState adapt;
    const auto at = root.getChildWithName ("Instrument");
    if (hasProfile && at.isValid() && (int) at.getProperty ("bands", 0) == tg::kNumBands)
    {
        juce::MemoryBlock block;
        if (decodeFloatBlock (at["measured"].toString(), adapt.measuredDb.size(), adapt.measuredDb.size(), block))
        {
            std::memcpy (adapt.measuredDb.data(), block.getData(), block.getSize());
            const bool finite = allPlausibleDb (adapt.measuredDb.data(), adapt.measuredDb.size());

            adapt.levelDb = at.getProperty ("level", -26.0f);
            adapt.levelSeconds = at.getProperty ("levelSeconds", 0.0f);
            adapt.tiltLow = at.getProperty ("tiltLow", 0.0f);
            adapt.tiltHigh = at.getProperty ("tiltHigh", 0.0f);
            adapt.tiltSeconds = at.getProperty ("tiltSeconds", 0.0f);
            adapt.measuredWeight = at.getProperty ("weight", 0.0f);
            adapt.valid = finite && std::isfinite (adapt.levelDb) && std::isfinite (adapt.levelSeconds)
                       && std::isfinite (adapt.tiltLow) && std::isfinite (adapt.tiltHigh)
                       && std::isfinite (adapt.tiltSeconds) && std::isfinite (adapt.measuredWeight);
        }
    }

    auditionPlaying.store (false);
    auditionAvailable.store (false);
    pushProfile (hasProfile ? &profile : nullptr, adapt.valid ? &adapt : nullptr);
}

//==============================================================================
juce::AudioProcessorEditor* ToneGenieProcessor::createEditor()
{
    return new ToneGenieEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ToneGenieProcessor();
}
