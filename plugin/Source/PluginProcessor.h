#pragma once

#include "Reference.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <tonegenie/ToneEngine.h>

namespace ParamIDs
{
    inline constexpr const char* drive = "drive";
    inline constexpr const char* voicing = "voicing";
    inline constexpr const char* comp = "comp";
    inline constexpr const char* gate = "gate";
    inline constexpr const char* match = "match";
    inline constexpr const char* adapt = "adapt";
    inline constexpr const char* reverbMix = "reverbMix";
    inline constexpr const char* reverbDecay = "reverbDecay";
    inline constexpr const char* delayMix = "delayMix";
    inline constexpr const char* delayTime = "delayTime";
    inline constexpr const char* delayFeedback = "delayFeedback";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* output = "output";
    inline constexpr const char* bypass = "bypass";
}

class ToneGenieProcessor : public juce::AudioProcessor,
                           private ReferenceManager::Listener
{
public:
    ToneGenieProcessor();
    ~ToneGenieProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ToneGenie"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // For the editor (message thread)
    juce::AudioProcessorValueTreeState apvts;
    ReferenceManager reference { *this };

    void loadReference (const juce::File& file, const ReferenceSettings& settings);
    void clearReference();
    void relearnInstrument() { relearnRequested.store (true); }

    bool hasAudition() const noexcept { return auditionAvailable.load(); }
    bool isAuditioning() const noexcept { return auditionPlaying.load(); }
    void setAuditioning (bool shouldPlay);

    void getSpectrum (tg::BandArray& target, tg::BandArray& output, tg::BandArray& eq) const { engine.getDisplay (target, output, eq); }
    float getInputTrimDb() const noexcept { return engine.inputGainDb(); }
    float getPickupCorrectionDb() const noexcept { return engine.pickupCorrectionDb(); }
    bool isPlaying() const noexcept { return engine.isPlaying(); }
    float getOutputLevelDb() const noexcept { return outputLevelDb.load (std::memory_order_relaxed); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void referenceAnalysed (const tg::ToneProfile& profile, std::vector<float>&& guitar, double guitarSampleRate) override;
    void pushProfile (const tg::ToneProfile* profile, const tg::AdaptState* adaptState);
    void setParam (const char* paramID, float plainValue);
    tg::EngineParams readParams (bool bypassed) const;
    void process (juce::AudioBuffer<float>& buffer, bool bypassed);

    /** Runs the matching-EQ design away from the audio thread. */
    class DesignThread : public juce::Thread
    {
    public:
        explicit DesignThread (tg::ToneEngine& e) : juce::Thread ("ToneGenie EQ design"), engine (e) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                engine.serviceBackground();
                wait (12);
            }
        }

    private:
        tg::ToneEngine& engine;
    };

    tg::ToneEngine engine;
    DesignThread designThread { engine };

    std::atomic<float>* pDrive = nullptr;
    std::atomic<float>* pVoicing = nullptr;
    std::atomic<float>* pComp = nullptr;
    std::atomic<float>* pGate = nullptr;
    std::atomic<float>* pMatch = nullptr;
    std::atomic<float>* pAdapt = nullptr;
    std::atomic<float>* pReverbMix = nullptr;
    std::atomic<float>* pReverbDecay = nullptr;
    std::atomic<float>* pDelayMix = nullptr;
    std::atomic<float>* pDelayTime = nullptr;
    std::atomic<float>* pDelayFeedback = nullptr;
    std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pOutput = nullptr;
    std::atomic<float>* pBypass = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    // message thread -> audio thread hand-over (audio thread only ever try-locks)
    juce::SpinLock pendingLock;
    tg::ToneProfile pendingProfile;
    tg::AdaptState pendingAdapt;
    bool pendingHasProfile = false, pendingHasAdapt = false;
    std::atomic<bool> pendingFlag { false };
    std::atomic<bool> relearnRequested { false };

    // "audition the isolated guitar" playback
    juce::SpinLock auditionLock;
    std::vector<float> auditionData;     // guarded by auditionLock
    double auditionRate = 44100.0;       // guarded by auditionLock
    double auditionPos = 0.0;            // audio thread only
    std::atomic<bool> auditionAvailable { false }, auditionPlaying { false }, auditionRewind { false };

    std::atomic<float> outputLevelDb { -100.0f };
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToneGenieProcessor)
};
