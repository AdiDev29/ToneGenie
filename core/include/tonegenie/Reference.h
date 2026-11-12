#pragma once

#include "Analysis.h"
#include "ToneProfile.h"

#include <string>
#include <vector>

namespace tg
{

//==============================================================================
// Is this file a guitar on its own, or a whole song?

struct MixFeatures
{
    float subDb = -100.0f;       ///< energy below the guitar's range (kick, bass) relative to total
    float airDb = -100.0f;       ///< energy above 9 kHz (cymbals, vocals' air) relative to total
    float occupancy = 0.0f;      ///< fraction of third-octave bands that carry real energy
    float sideDb = -100.0f;      ///< stereo width
    float levelDb = -100.0f;
};

struct MixVerdict
{
    bool fullMix = false;
    float probability = 0.0f;    ///< 0 = certainly a lone guitar, 1 = certainly a full mix
    MixFeatures features;
};

struct MixSegment
{
    double startSec = 0.0, endSec = 0.0;
    MixVerdict verdict;
};

/** Pre-computed per-frame statistics, so whole-file and per-segment verdicts are cheap. */
class MixAnalyzer
{
public:
    /** right may be null for mono material. Returns false if cancelled. */
    bool analyze (const float* left, const float* right, size_t numSamples, double sampleRate, const ProgressFn& progress = {});

    MixVerdict verdict() const { return verdictForRange (0, numFrames); }
    std::vector<MixSegment> segments (double segmentSeconds) const;

private:
    MixVerdict verdictForRange (int firstFrame, int endFrame) const;

    static constexpr int kThirdBands = 26;

    double sr = 44100.0;
    int hop = 2048, numFrames = 0;
    std::vector<float> total, sub, air, mid, side;
    std::vector<float> thirds; // numFrames x kThirdBands
};

//==============================================================================
// Pulling the guitar out of a song.

enum class Focus
{
    automatic,
    left,
    center,
    right
};

struct IsolationResult
{
    bool ok = false;
    std::vector<float> guitar;   ///< mono estimate
    float pan = 0.0f;            ///< -1..+1 position that was extracted
    float validLoHz = 110.0f, validHiHz = 7000.0f;
};

/** Classic DSP source separation aimed at "what does the guitar sound like"
    rather than at a release-quality stem:
      - harmonic/percussive median filtering removes drums,
      - stereo position isolates a panned guitar and cancels everything that
        sits in the middle (vocals, bass, kick, snare),
      - band limiting removes what is left of bass and cymbals. */
IsolationResult isolateGuitar (const float* left, const float* right, size_t numSamples, double sampleRate,
                               Focus focus, const ProgressFn& progress = {});

//==============================================================================
// The whole "give it an audio file" pipeline.

enum class SourceMode
{
    automatic,
    guitarOnly,
    fullMix
};

struct ReferenceOptions
{
    SourceMode mode = SourceMode::automatic;
    Focus focus = Focus::automatic;
    double regionStartSec = -1.0, regionEndSec = -1.0; ///< negative = choose automatically
};

struct ReferenceResult
{
    bool ok = false;
    std::string message;          ///< human-readable summary or error
    ToneProfile profile;
    std::vector<float> guitar;    ///< the guitar signal the tone was taken from (for auditioning)
    double guitarSampleRate = 44100.0;
};

ReferenceResult analyzeReference (const float* left, const float* right, size_t numSamples, double sampleRate,
                                  const ReferenceOptions& options, const ProgressFn& progress = {});

} // namespace tg
