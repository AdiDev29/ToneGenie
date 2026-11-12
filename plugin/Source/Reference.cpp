#include "Reference.h"

#include <cstring>

namespace
{
    constexpr double maxFileMinutes = 12.0;
    constexpr int thumbPoints = 1400;

    juce::String floatsToString (const float* data, size_t count)
    {
        return juce::MemoryBlock (data, count * sizeof (float)).toBase64Encoding();
    }

    bool stringToFloats (const juce::String& text, float* dest, size_t count)
    {
        juce::MemoryBlock block;
        if (! decodeFloatBlock (text, count, count, block))
            return false;
        std::memcpy (dest, block.getData(), block.getSize());
        return true;
    }

    std::vector<float> stringToFloatVector (const juce::String& text)
    {
        juce::MemoryBlock block;
        std::vector<float> v;
        if (decodeFloatBlock (text, 1, 16384, block))
        {
            v.resize (block.getSize() / sizeof (float));
            std::memcpy (v.data(), block.getData(), block.getSize());
        }
        return v;
    }

    /** Reads a number from saved state: never NaN/Inf, always inside [lo, hi]. */
    float num (const juce::ValueTree& tree, const char* property, float fallback, float lo, float hi)
    {
        const auto v = (float) (double) tree.getProperty (property, fallback);
        return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback;
    }

    bool allFinite (const float* data, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            if (! std::isfinite (data[i]))
                return false;
        return true;
    }
}

ReferenceManager::ReferenceManager (Listener& listenerToUse)
    : juce::Thread ("ToneGenie analysis"), listener (listenerToUse)
{
    snapshot.status = "Drop an audio file here, or click Load, to capture its guitar tone.";
}

ReferenceManager::~ReferenceManager()
{
    shutdown();
}

void ReferenceManager::shutdown()
{
    stopThread (15000);
    cancelPendingUpdate();

    // A completion callback may already be running on the message thread while
    // we are being destroyed on another one (some plugin formats do that): wait
    // for it, and turn any later one into a no-op.
    const juce::ScopedLock cb (callbackLock);
    dead = true;
}

juce::String ReferenceManager::getFileWildcard()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats();
}

bool ReferenceManager::isSupportedFile (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return file.existsAsFile() && formats.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

void ReferenceManager::analyse (const juce::File& file, const ReferenceSettings& settings)
{
    stopThread (15000);
    cancelPendingUpdate();

    {
        const juce::ScopedLock sl (lock);
        ++generation;
        hasFinished = false;
        jobFile = file;
        jobSettings = settings;

        if (snapshot.filePath != file.getFullPathName())
        {
            snapshot.thumbMin.clear();
            snapshot.thumbMax.clear();
            snapshot.duration = 0.0;
        }

        snapshot.filePath = file.getFullPathName();
        snapshot.settings = settings;
        snapshot.status = "Loading " + file.getFileName() + "...";
    }

    progress.store (0.0f);
    busy.store (true);
    ++revision;
    startThread (juce::Thread::Priority::low);
}

void ReferenceManager::clear()
{
    stopThread (15000);
    cancelPendingUpdate();

    {
        const juce::ScopedLock sl (lock);
        ++generation;
        snapshot = ReferenceSnapshot();
        snapshot.status = "Drop an audio file here, or click Load, to capture its guitar tone.";
        hasFinished = false;
    }

    busy.store (false);
    ++revision;
}

ReferenceSnapshot ReferenceManager::getSnapshot() const
{
    const juce::ScopedLock sl (lock);
    return snapshot;
}

void ReferenceManager::run()
{
    juce::File file;
    ReferenceSettings settings;
    int job = 0;
    {
        const juce::ScopedLock sl (lock);
        file = jobFile;
        settings = jobSettings;
        job = generation.load();
    }

    // Anything this job reports is dropped if it has been superseded meanwhile
    // (a new file, Clear, or the host restoring a saved state).
    const auto fail = [this, job] (const juce::String& message)
    {
        const juce::ScopedLock sl (lock);
        if (job != generation.load())
            return;
        busy.store (false);
        snapshot.status = message;
        ++revision;
    };

    // --- decode ---------------------------------------------------------------
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return fail ("Could not read \"" + file.getFileName() + "\". Supported formats: " + formats.getWildcardForAllFormats());

    const double sampleRate = reader->sampleRate;
    const auto maxSamples = (juce::int64) (maxFileMinutes * 60.0 * sampleRate);
    const bool truncated = reader->lengthInSamples > maxSamples;
    const int numSamples = (int) juce::jmin (reader->lengthInSamples, maxSamples);
    const int numChannels = reader->numChannels > 1 ? 2 : 1;

    juce::AudioBuffer<float> audio;
    audio.setSize (numChannels, numSamples);

    // read in slices so a cancel request is honoured quickly
    for (int pos = 0; pos < numSamples;)
    {
        if (threadShouldExit())
            return;

        const int n = juce::jmin (1 << 18, numSamples - pos);
        if (! reader->read (&audio, pos, n, pos, true, numChannels > 1))
            return fail ("Could not decode \"" + file.getFileName() + "\".");
        pos += n;
    }
    reader.reset();

    const float* left = audio.getReadPointer (0);
    const float* right = numChannels > 1 ? audio.getReadPointer (1) : nullptr;

    // --- waveform overview ----------------------------------------------------------
    {
        std::vector<float> mn ((size_t) thumbPoints, 0.0f), mx ((size_t) thumbPoints, 0.0f);

        for (int p = 0; p < thumbPoints; ++p)
        {
            const int a = (int) ((juce::int64) p * numSamples / thumbPoints);
            const int b = juce::jmax (a + 1, (int) ((juce::int64) (p + 1) * numSamples / thumbPoints));
            float lo = 0.0f, hi = 0.0f;
            for (int i = a; i < b && i < numSamples; ++i)
            {
                const float v = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
                lo = juce::jmin (lo, v);
                hi = juce::jmax (hi, v);
            }
            mn[(size_t) p] = lo;
            mx[(size_t) p] = hi;
        }

        const juce::ScopedLock sl (lock);
        if (job != generation.load())
            return;
        snapshot.thumbMin = std::move (mn);
        snapshot.thumbMax = std::move (mx);
        snapshot.duration = numSamples / sampleRate;
        snapshot.status = "Listening to " + file.getFileName() + "...";
    }
    ++revision;

    // --- analyse ----------------------------------------------------------------------
    tg::ReferenceOptions options;
    options.mode = settings.sourceMode == 1 ? tg::SourceMode::guitarOnly
                 : settings.sourceMode == 2 ? tg::SourceMode::fullMix : tg::SourceMode::automatic;
    options.focus = settings.focus == 1 ? tg::Focus::left
                  : settings.focus == 2 ? tg::Focus::center
                  : settings.focus == 3 ? tg::Focus::right : tg::Focus::automatic;
    options.regionStartSec = settings.regionStart;
    options.regionEndSec = settings.regionEnd;

    auto result = tg::analyzeReference (left, right, (size_t) numSamples, sampleRate, options, [this, job] (float p)
    {
        progress.store (p);
        return ! threadShouldExit() && job == generation.load();
    });

    if (threadShouldExit() || job != generation.load())
        return;

    if (truncated && result.ok)
        result.message += " (Only the first " + std::to_string ((int) maxFileMinutes) + " minutes were used.)";

    {
        const juce::ScopedLock sl (lock);
        if (job != generation.load())
            return;
        finished = std::move (result);
        finishedGeneration = job;
        hasFinished = true;
    }

    triggerAsyncUpdate();
}

void ReferenceManager::handleAsyncUpdate()
{
    const juce::ScopedLock cb (callbackLock);
    if (dead)
        return;

    tg::ReferenceResult result;
    {
        const juce::ScopedLock sl (lock);
        if (! hasFinished)
            return;

        hasFinished = false;
        result = std::move (finished);
        finished = {};

        if (finishedGeneration != generation.load())
            return; // superseded while the completion message was in flight

        snapshot.status = result.message.empty() ? juce::String ("Analysis failed.") : juce::String (result.message);
        if (result.ok)
        {
            snapshot.profile = result.profile;
            snapshot.hasProfile = true;
        }
    }

    busy.store (false);
    ++revision;

    if (result.ok)
        listener.referenceAnalysed (result.profile, std::move (result.guitar), result.guitarSampleRate);
}

//==============================================================================
juce::ValueTree ReferenceManager::toValueTree() const
{
    const juce::ScopedLock sl (lock);

    juce::ValueTree tree ("Reference");
    tree.setProperty ("file", snapshot.filePath, nullptr);
    tree.setProperty ("status", snapshot.status, nullptr);
    tree.setProperty ("sourceMode", snapshot.settings.sourceMode, nullptr);
    tree.setProperty ("focus", snapshot.settings.focus, nullptr);
    tree.setProperty ("regionStart", snapshot.settings.regionStart, nullptr);
    tree.setProperty ("regionEnd", snapshot.settings.regionEnd, nullptr);
    tree.setProperty ("duration", snapshot.duration, nullptr);

    if (! snapshot.thumbMin.empty() && snapshot.thumbMin.size() == snapshot.thumbMax.size())
    {
        tree.setProperty ("thumbMin", floatsToString (snapshot.thumbMin.data(), snapshot.thumbMin.size()), nullptr);
        tree.setProperty ("thumbMax", floatsToString (snapshot.thumbMax.data(), snapshot.thumbMax.size()), nullptr);
    }

    if (snapshot.hasProfile)
    {
        const auto& p = snapshot.profile;
        juce::ValueTree pt ("Profile");
        pt.setProperty ("bands", tg::kNumBands, nullptr);
        pt.setProperty ("target", floatsToString (p.targetDb.data(), p.targetDb.size()), nullptr);
        pt.setProperty ("expected", floatsToString (p.expectedPreDb.data(), p.expectedPreDb.size()), nullptr);
        pt.setProperty ("validLo", p.validLoHz, nullptr);
        pt.setProperty ("validHi", p.validHiHz, nullptr);
        pt.setProperty ("tilt", p.canonicalTiltDb, nullptr);
        pt.setProperty ("voicing", p.voicing, nullptr);
        pt.setProperty ("drive", p.drive, nullptr);
        pt.setProperty ("comp", p.comp, nullptr);
        pt.setProperty ("gate", p.gate, nullptr);
        pt.setProperty ("reverbMix", p.reverbMix, nullptr);
        pt.setProperty ("reverbDecay", p.reverbDecay, nullptr);
        pt.setProperty ("fitError", p.fitError, nullptr);
        pt.setProperty ("fromFullMix", p.fromFullMix, nullptr);
        pt.setProperty ("exposed", p.usedExposedGuitar, nullptr);
        pt.setProperty ("isolated", p.isolated, nullptr);
        pt.setProperty ("mixProbability", p.mixProbability, nullptr);
        pt.setProperty ("pan", p.isolationPan, nullptr);
        pt.setProperty ("usedStart", p.usedStartSec, nullptr);
        pt.setProperty ("usedEnd", p.usedEndSec, nullptr);
        pt.setProperty ("crest", p.reference.crestDb, nullptr);
        pt.setProperty ("sustain", p.reference.sustainDbPerSec, nullptr);
        pt.setProperty ("spread", p.reference.levelSpreadDb, nullptr);
        tree.appendChild (pt, nullptr);
    }

    return tree;
}

bool ReferenceManager::fromValueTree (const juce::ValueTree& tree, tg::ToneProfile& profileOut)
{
    ReferenceSnapshot s;
    s.status = "Drop an audio file here, or click Load, to capture its guitar tone.";

    if (tree.isValid())
    {
        s.filePath = tree["file"].toString();
        if (tree.hasProperty ("status"))
            s.status = tree["status"].toString();
        s.settings.sourceMode = juce::jlimit (0, 2, (int) tree.getProperty ("sourceMode", 0));
        s.settings.focus = juce::jlimit (0, 3, (int) tree.getProperty ("focus", 0));
        s.settings.regionStart = num (tree, "regionStart", -1.0f, -1.0f, 1.0e6f);
        s.settings.regionEnd = num (tree, "regionEnd", -1.0f, -1.0f, 1.0e6f);
        s.duration = num (tree, "duration", 0.0f, 0.0f, 1.0e6f);
        s.thumbMin = stringToFloatVector (tree["thumbMin"].toString());
        s.thumbMax = stringToFloatVector (tree["thumbMax"].toString());

        if (s.thumbMin.size() != s.thumbMax.size() || ! allFinite (s.thumbMin.data(), s.thumbMin.size())
            || ! allFinite (s.thumbMax.data(), s.thumbMax.size()))
        {
            s.thumbMin.clear();
            s.thumbMax.clear();
        }

        const auto pt = tree.getChildWithName ("Profile");
        if (pt.isValid() && (int) pt.getProperty ("bands", 0) == tg::kNumBands)
        {
            tg::ToneProfile p;
            const bool arraysOk = stringToFloats (pt["target"].toString(), p.targetDb.data(), p.targetDb.size())
                               && stringToFloats (pt["expected"].toString(), p.expectedPreDb.data(), p.expectedPreDb.size())
                               && allPlausibleDb (p.targetDb.data(), p.targetDb.size())
                               && allPlausibleDb (p.expectedPreDb.data(), p.expectedPreDb.size());

            if (arraysOk)
            {
                p.validLoHz = num (pt, "validLo", 60.0f, 20.0f, 2000.0f);
                p.validHiHz = num (pt, "validHi", 16000.0f, 2000.0f, 20000.0f);
                p.canonicalTiltDb = num (pt, "tilt", 0.0f, -40.0f, 40.0f);
                p.voicing = juce::jlimit (0, (int) tg::numVoicings - 1, (int) pt.getProperty ("voicing", 1));
                p.drive = num (pt, "drive", 0.5f, 0.0f, 1.0f);
                p.comp = num (pt, "comp", 0.0f, 0.0f, 1.0f);
                p.gate = num (pt, "gate", 0.2f, 0.0f, 1.0f);
                p.reverbMix = num (pt, "reverbMix", 0.0f, 0.0f, 1.0f);
                p.reverbDecay = num (pt, "reverbDecay", 1.2f, 0.2f, 8.0f);
                p.fitError = num (pt, "fitError", 0.0f, 0.0f, 1.0e6f);
                p.fromFullMix = pt.getProperty ("fromFullMix", false);
                p.usedExposedGuitar = pt.getProperty ("exposed", false);
                p.isolated = pt.getProperty ("isolated", false);
                p.mixProbability = num (pt, "mixProbability", 0.0f, 0.0f, 1.0f);
                p.isolationPan = num (pt, "pan", 0.0f, -1.0f, 1.0f);
                p.usedStartSec = num (pt, "usedStart", 0.0f, 0.0f, 1.0e6f);
                p.usedEndSec = num (pt, "usedEnd", 0.0f, 0.0f, 1.0e6f);
                p.reference.crestDb = num (pt, "crest", 0.0f, 0.0f, 100.0f);
                p.reference.sustainDbPerSec = num (pt, "sustain", 0.0f, -200.0f, 20.0f);
                p.reference.levelSpreadDb = num (pt, "spread", 0.0f, 0.0f, 200.0f);
                p.reference.ltasDb = p.targetDb;
                p.valid = true;

                s.profile = p;
                s.hasProfile = true;
            }
        }
    }

    {
        // A restored state replaces whatever an analysis in flight would have produced.
        const juce::ScopedLock sl (lock);
        ++generation;
        hasFinished = false;
        snapshot = s;
    }
    signalThreadShouldExit();
    busy.store (false);
    ++revision;

    profileOut = s.profile;
    return s.hasProfile;
}
