#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <tonegenie/Reference.h>

#include <cmath>

/** Decodes a block of floats saved with MemoryBlock::toBase64Encoding ("<bytes>.<data>").
    The declared size is checked first, so damaged state data cannot trigger a huge allocation. */
inline bool decodeFloatBlock (const juce::String& text, size_t minFloats, size_t maxFloats, juce::MemoryBlock& block)
{
    const auto prefix = text.upToFirstOccurrenceOf (".", false, false);
    if (prefix.isEmpty() || prefix.length() == text.length() || prefix.length() > 9 || ! prefix.containsOnly ("0123456789"))
        return false;

    const auto bytes = (size_t) prefix.getLargeIntValue();
    if (bytes % sizeof (float) != 0 || bytes < minFloats * sizeof (float) || bytes > maxFloats * sizeof (float))
        return false;

    return block.fromBase64Encoding (text) && block.getSize() == bytes;
}

/** True if every value is a plausible level in dB (finite and within sane bounds). */
inline bool allPlausibleDb (const float* data, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (! (std::isfinite (data[i]) && data[i] >= -250.0f && data[i] <= 100.0f))
            return false;
    return true;
}

/** How the user wants the reference file to be treated. */
struct ReferenceSettings
{
    int sourceMode = 0;   ///< 0 auto, 1 guitar only, 2 full mix
    int focus = 0;        ///< 0 auto, 1 left, 2 centre, 3 right
    double regionStart = -1.0, regionEnd = -1.0; ///< seconds; negative = whole file / automatic
};

/** Everything the UI needs to know about the reference tone. */
struct ReferenceSnapshot
{
    juce::String filePath, status;
    bool hasProfile = false;
    tg::ToneProfile profile;
    std::vector<float> thumbMin, thumbMax; ///< waveform overview
    double duration = 0.0;
    ReferenceSettings settings;
};

/** Loads reference audio files and analyses them on a background thread. */
class ReferenceManager : private juce::Thread,
                         private juce::AsyncUpdater
{
public:
    struct Listener
    {
        virtual ~Listener() = default;

        /** Called on the message thread when a new tone has been learned. */
        virtual void referenceAnalysed (const tg::ToneProfile& profile, std::vector<float>&& guitar, double guitarSampleRate) = 0;
    };

    explicit ReferenceManager (Listener& listenerToUse);
    ~ReferenceManager() override;

    /** Starts (or restarts) the analysis of a file. Message thread. */
    void analyse (const juce::File& file, const ReferenceSettings& settings);

    /** Forgets the reference tone. */
    void clear();

    /** Stops the analysis and guarantees that no further callback reaches the
        listener. Call before the listener starts being destroyed. Any thread. */
    void shutdown();

    ReferenceSnapshot getSnapshot() const;
    int getRevision() const noexcept { return revision.load(); }
    bool isBusy() const noexcept { return busy.load(); }
    float getProgress() const noexcept { return progress.load(); }

    juce::ValueTree toValueTree() const;

    /** Restores a saved reference. Returns true if it contained a valid profile. Any thread. */
    bool fromValueTree (const juce::ValueTree& tree, tg::ToneProfile& profileOut);

    static juce::String getFileWildcard();
    static bool isSupportedFile (const juce::File& file);

private:
    void run() override;
    void handleAsyncUpdate() override;

    Listener& listener;

    juce::CriticalSection callbackLock;  ///< held while a completion callback runs
    bool dead = false;                   // guarded by callbackLock

    mutable juce::CriticalSection lock;
    ReferenceSnapshot snapshot;          // guarded by lock
    juce::File jobFile;                  // guarded by lock
    ReferenceSettings jobSettings;       // guarded by lock
    tg::ReferenceResult finished;        // guarded by lock
    bool hasFinished = false;            // guarded by lock

    std::atomic<int> generation { 0 };   ///< bumped whenever a running job's result becomes unwanted
    int finishedGeneration = -1;         // guarded by lock
    std::atomic<int> revision { 0 };
    std::atomic<bool> busy { false };
    std::atomic<float> progress { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceManager)
};
