#pragma once

#include "FrontEnd.h"
#include "MatchEq.h"
#include "ToneProfile.h"

namespace tg
{

/** Feedback delay with a darkening filter in the loop. */
class EchoDelay
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void set (float timeMs, float feedback, float mix) noexcept;
    void process (float* x, int n) noexcept;

    /** Jump straight to the current settings instead of gliding there. */
    void snap() noexcept { timeCur = timeTarget; mixCur = mixTarget; }

private:
    double sr = 44100.0;
    DelayLine line;
    OnePole damp;
    float timeTarget = 1000.0f, timeCur = 1000.0f, timeCoeff = 0.001f;
    float fb = 0.3f, mixTarget = 0.0f, mixCur = 0.0f;
};

/** Compact 8-line feedback delay network reverb (mono in, stereo out). */
class Reverb
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void set (float mix, float decaySeconds) noexcept;

    /** Reads mono `in`, writes dry+wet to outL/outR. */
    void process (const float* in, float* outL, float* outR, int n) noexcept;

private:
    static constexpr int kLines = 8;

    double sr = 44100.0;
    DelayLine pre, ap1, ap2;
    DelayLine lines[kLines];
    OnePole lineDamp[kLines];
    OnePole inHp;
    int lineLen[kLines] {};
    int ap1Len = 1, ap2Len = 1, preLen = 1;
    float lineGain[kLines] {};
    float mixTarget = 0.0f, mixCur = 0.0f, decay = -1.0f;
    bool idle = false;
};

struct EngineParams
{
    int voicing = voicingCrunch;
    float drive = 0.5f;         ///< 0..1
    float comp = 0.0f;          ///< 0..1
    float gate = 0.2f;          ///< 0..1
    float match = 1.0f;         ///< 0..1 how much of the matching EQ is applied
    bool adapt = true;          ///< keep learning the instrument
    float reverbMix = 0.0f;     ///< 0..1
    float reverbDecay = 1.2f;   ///< seconds
    float delayMix = 0.0f;      ///< 0..1
    float delayTimeMs = 380.0f;
    float delayFeedback = 0.3f; ///< 0..0.9
    float mix = 1.0f;           ///< dry/wet
    float outputDb = 0.0f;
};

/** What the engine has learned about the instrument plugged into it. */
struct AdaptState
{
    bool valid = false;
    float levelDb = -26.0f, levelSeconds = 0.0f;
    float tiltLow = 0.0f, tiltHigh = 0.0f, tiltSeconds = 0.0f;
    BandArray measuredDb {};
    float measuredWeight = 0.0f;
};

/** The complete effect: clean guitar in, matched tone out. */
class ToneEngine
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    // --- audio thread ---------------------------------------------------------
    void setParams (const EngineParams& p) noexcept;
    void setProfile (const ToneProfile* profile) noexcept; ///< nullptr = no reference tone
    void setAdaptState (const AdaptState& s) noexcept;
    void relearn() noexcept;

    /** inR / outR may be null (mono). in and out may alias. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    // --- any thread -------------------------------------------------------------
    static int latencySamples() noexcept { return FrontEnd::kLatency; }

    /** Runs the non-realtime filter design. Call regularly from a non-audio thread. */
    bool serviceBackground() { return eq.service(); }

    /** Design filters inside process() instead (offline rendering, tests). */
    void setSynchronous (bool sync) noexcept { eq.setSynchronous (sync); }

    /** Designs and installs the matching filter now, without a cross-fade.
        Only while process() is not running (offline tools). */
    void designNow() { eq.designNow(); }

    AdaptState getAdaptState() const noexcept;
    void getDisplay (BandArray& targetDb, BandArray& outputDb, BandArray& eqDb) const noexcept { eq.getDisplay (targetDb, outputDb, eqDb); }
    float inputGainDb() const noexcept { return meterInputGain.load (std::memory_order_relaxed); }
    float pickupCorrectionDb() const noexcept { return meterTilt.load (std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return meterActive.load (std::memory_order_relaxed); }

private:
    double sr = 44100.0;
    EngineParams params;
    FrontEnd front;
    MatchEq eq;
    EchoDelay delay;
    Reverb reverb;
    DelayLine dryL, dryR;
    float mixCur = 1.0f, outCur = 1.0f;

    std::atomic<float> meterInputGain { 0.0f }, meterTilt { 0.0f };
    std::atomic<float> stLevel { -26.0f }, stLevelSecs { 0.0f }, stTiltLow { 0.0f }, stTiltHigh { 0.0f }, stTiltSecs { 0.0f };
    std::atomic<bool> meterActive { false };
};

} // namespace tg
