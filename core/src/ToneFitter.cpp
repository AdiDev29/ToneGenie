#include "tonegenie/ToneFitter.h"
#include "tonegenie/FrontEnd.h"
#include "tonegenie/MatchEq.h"
#include "tonegenie/SyntheticGuitar.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace tg
{

ToneFitter::ToneFitter()
{
    double length = 0.0;
    const auto phrase = calibrationPhrase (length);
    const auto guitar = renderGuitar (phrase, kSampleRate, length, GuitarVoice());

    // Run the same input conditioning the live engine uses, twice: the first
    // pass lets the leveler settle, the second pass is the calibration signal.
    InputLeveler leveler;
    PickupTilt tilt;
    leveler.prepare (kSampleRate);
    tilt.prepare (kSampleRate);
    tilt.setTarget (0.0f, false);

    levelled.resize (guitar.size());

    for (int pass = 0; pass < 2; ++pass)
    {
        for (size_t pos = 0; pos < guitar.size(); pos += kMaxChunk)
        {
            const int n = (int) std::min ((size_t) kMaxChunk, guitar.size() - pos);
            float chunk[kMaxChunk];
            std::copy (guitar.begin() + (std::ptrdiff_t) pos, guitar.begin() + (std::ptrdiff_t) pos + n, chunk);

            const bool active = leveler.process (chunk, n);
            tilt.process (chunk, n, active);

            if (pass == 1)
                std::copy (chunk, chunk + n, levelled.begin() + (std::ptrdiff_t) pos);
        }
    }

    canonicalTilt = tilt.measuredDb();
}

float ToneFitter::distance (const ToneFeatures& c, const ToneFeatures& r, float crestWeight) noexcept
{
    const auto sq = [] (float v) { return v * v; };

    // The waveform's crest factor is the most playing-independent sign of
    // clipping, as long as the reference guitar signal is reasonably clean.
    const float wCrest = crestWeight;
    float d = wCrest * sq (c.crestDb - r.crestDb);

    // Sustain and level spread also depend on *what* is played: staccato or
    // fast-decaying parts look "cleaner" than they are. The reverse cannot
    // happen - nothing but saturation makes notes hold their level - so a
    // candidate that sustains less than the reference is penalised in full,
    // one that sustains more only lightly.
    const auto oneSided = [&] (float candidateMinusReference)
    {
        return candidateMinusReference > 0.0f ? sq (candidateMinusReference) : 0.2f * sq (candidateMinusReference);
    };

    d += 0.4f * oneSided ((c.levelSpreadDb - r.levelSpreadDb) / 4.0f);

    if (c.sustainValid && r.sustainValid)
    {
        // decay rates on a compressive scale: 0 vs -3 dB/s matters, -30 vs -40 does not
        const auto warp = [] (float slope) { return std::log2 (1.0f + std::max (0.0f, -slope)); };
        d += 0.6f * oneSided (warp (c.sustainDbPerSec) - warp (r.sustainDbPerSec));
    }

    return d;
}

ToneFitter::Candidate ToneFitter::evaluate (int voicing, float drive, float comp, const ToneFeatures& reference,
                                            float validLoHz, float validHiHz, float crestWeight,
                                            std::vector<float>* renderedOutput) const
{
    Candidate c;
    c.voicing = voicing;
    c.drive = drive;
    c.comp = comp;

    std::vector<float> sig (levelled);

    Compressor compressor;
    DriveStage stage;
    compressor.prepare (kSampleRate);
    compressor.setAmount (comp);
    stage.prepare (kSampleRate);
    stage.set (voicing, drive);
    stage.prepare (kSampleRate); // snap the gain smoother to its target

    for (size_t pos = 0; pos < sig.size(); pos += kMaxChunk)
    {
        const int n = (int) std::min ((size_t) kMaxChunk, sig.size() - pos);
        compressor.process (sig.data() + pos, n);
        stage.process (sig.data() + pos, n);
    }

    c.preDb = analyzeTone (sig.data(), sig.size(), kSampleRate).ltasDb;

    MatchEq eq;
    eq.prepare (kSampleRate);
    eq.setAdapting (false);
    eq.setTarget (reference.ltasDb, c.preDb, validLoHz, validHiHz);
    eq.designNow();

    for (size_t pos = 0; pos < sig.size(); pos += kMaxChunk)
    {
        const int n = (int) std::min ((size_t) kMaxChunk, sig.size() - pos);
        eq.process (sig.data() + pos, n, true);
    }

    const auto frames = analyzeFrames (sig.data(), sig.size(), kSampleRate);
    c.features = summarizeTone (frames);

    // Compare like with like: use the part of the calibration phrase that is
    // as dense (single notes vs chords) as the reference's playing.
    c.features.crestDb = crestAtDensity (frames, reference.density);
    c.distance = distance (c.features, reference, crestWeight);

    if (renderedOutput != nullptr)
        *renderedOutput = std::move (sig);

    return c;
}

bool ToneFitter::fit (ToneProfile& profile, const ProgressFn& progress) const
{
    const auto& ref = profile.reference;

    // Source separation smears the waveform: a guitar pulled out by its stereo
    // position is still fairly intact, one dug out of the centre is not.
    const float crestWeight = ! profile.isolated ? 1.0f
                            : std::abs (profile.isolationPan) > 0.1f ? 0.6f : 0.1f;

    static const float prior[numVoicings] = { 0.04f, 0.0f, 0.12f };
    static const float grid[] = { 0.04f, 0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 0.96f };
    const int gridSize = (int) (sizeof (grid) / sizeof (grid[0]));
    const int totalSteps = numVoicings * gridSize + 2 + 3;
    int step = 0;

    const auto report = [&]
    {
        ++step;
        return ! progress || progress ((float) step / (float) totalSteps);
    };

    const auto eval = [&] (int v, float d, float comp)
    {
        auto c = evaluate (v, d, comp, ref, profile.validLoHz, profile.validHiHz, crestWeight);
        c.distance += prior[v];
        return c;
    };

    Candidate best;
    best.distance = 1.0e9f;

    // Coarse grid over every voicing, evaluated on a few worker threads.
    {
        const int numJobs = numVoicings * gridSize;
        std::vector<Candidate> results ((size_t) numJobs);
        std::atomic<int> next { 0 };
        std::atomic<bool> cancelled { false };
        std::mutex progressLock;

        const auto worker = [&]
        {
            for (;;)
            {
                const int job = next.fetch_add (1);
                if (job >= numJobs || cancelled.load())
                    return;

                try
                {
                    results[(size_t) job] = eval (job / gridSize, grid[job % gridSize], 0.0f);

                    const std::lock_guard<std::mutex> lock (progressLock);
                    if (! report())
                        cancelled.store (true);
                }
                catch (...)
                {
                    // out of memory etc.: give up cleanly instead of terminating the host
                    cancelled.store (true);
                    return;
                }
            }
        };

        const int numThreads = (int) std::clamp (std::thread::hardware_concurrency(), 1u, 4u);
        std::vector<std::thread> threads;
        try
        {
            for (int i = 1; i < numThreads; ++i)
                threads.emplace_back (worker);
        }
        catch (...)
        {
            // could not start (more) threads: the ones we have, plus this one, do the work
        }
        worker();
        for (auto& t : threads)
            t.join();

        if (cancelled.load())
            return false;

        for (const auto& c : results)
            if (c.distance < best.distance)
                best = c;
    }

    // refine the drive around the best grid point
    const float centre = best.drive;
    const int centreVoicing = best.voicing;

    for (float delta : { -0.075f, 0.075f })
    {
        const float d = std::clamp (centre + delta, 0.0f, 1.0f);
        const auto c = eval (centreVoicing, d, 0.0f);
        if (c.distance < best.distance)
            best = c;
        if (! report())
            return false;
    }

    // A clean-ish tone that is still more even than ours: add compression.
    if (best.features.levelSpreadDb > ref.levelSpreadDb + 1.5f
        && DriveStage::gainDbFor (best.voicing, best.drive) < 22.0f)
    {
        for (float comp : { 0.3f, 0.6f, 0.9f })
        {
            auto c = eval (best.voicing, best.drive, comp);
            c.distance += 0.02f * comp;
            if (c.distance < best.distance)
                best = c;
            if (! report())
                return false;
        }
    }

    profile.voicing = best.voicing;
    profile.drive = best.drive;
    profile.comp = best.comp;
    profile.expectedPreDb = best.preDb;
    profile.canonicalTiltDb = canonicalTilt;
    profile.fitError = best.distance;

    // More gain needs more gating.
    const float gainDb = DriveStage::gainDbFor (best.voicing, best.drive);
    profile.gate = std::clamp (0.1f + 0.4f * (gainDb - 10.0f) / 40.0f, 0.1f, 0.55f);

    if (progress)
        progress (1.0f);
    return true;
}

} // namespace tg
