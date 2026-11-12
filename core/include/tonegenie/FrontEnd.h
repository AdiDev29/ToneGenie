#pragma once

#include "DspBlocks.h"
#include "ToneProfile.h"

#include <atomic>

namespace tg
{

/** 4x polyphase FIR oversampler (linear phase, 32 samples of latency at base rate). */
class Oversampler4x
{
public:
    static constexpr int kFactor = 4;
    static constexpr int kTaps = 129;
    static constexpr int kPhaseTaps = 33;
    static constexpr int kLatency = (kTaps - 1) / kFactor;

    Oversampler4x();
    void reset() noexcept;

    /** in: n samples (n <= kMaxChunk), out: 4n samples. */
    void up (const float* in, float* out, int n) noexcept;

    /** in: 4n samples, out: n samples (n <= kMaxChunk). */
    void down (const float* in, float* out, int n) noexcept;

private:
    float h[kTaps] {};
    float phase[kFactor][kPhaseTaps] {};
    float upBuf[kPhaseTaps - 1 + kMaxChunk] {};
    float downBuf[kTaps - 1 + kFactor * kMaxChunk] {};
};

/** Tracks the level a player is actually producing and normalises it, so every
    guitar/pickup hits the drive stage equally hard. Slow by design: it follows
    the instrument, not the notes. */
class InputLeveler
{
public:
    static constexpr float kReferenceDb = -20.0f;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }

    /** In-place, n <= kMaxChunk. Returns true if the chunk contained playing. */
    bool process (float* x, int n) noexcept;

    float trackedDb() const noexcept { return level; }
    float activeSeconds() const noexcept { return activeTime; }
    float gainDb() const noexcept { return targetGainDb; }
    void setState (float trackedLevelDb, float activeSecs) noexcept;

private:
    double sr = 44100.0;
    float msCoeff = 0.0f, ms = 0.0f;
    float level = -26.0f, activeTime = 0.0f;
    float targetGainDb = 6.0f, gain = 2.0f;
    bool frozen = false;
};

/** Measures how bright the instrument is and tilts it towards the calibration
    guitar the profile was fitted with (single coils vs humbuckers etc.). */
class PickupTilt
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setTarget (float canonicalTiltDb, bool enabled) noexcept { target = canonicalTiltDb; on = enabled; }
    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }

    void process (float* x, int n, bool active) noexcept;

    /** Brightness of the incoming signal (high band minus low band, dB). */
    float measuredDb() const noexcept;
    float correctionDb() const noexcept { return corrDb; }
    void setState (float lowPower, float highPower, float weightSecs) noexcept;
    void getState (float& lowPower, float& highPower, float& weightSecs) const noexcept;

private:
    void updateFilter() noexcept;

    double sr = 44100.0;
    Biquad bpLow, bpHigh, shelf;
    float msLow = 0, msHigh = 0, msCoeff = 0;
    float avgLow = 0, avgHigh = 0, weight = 0;
    float target = 0, corrDb = 0, appliedDb = 0, broadband = 1.0f;
    bool on = false, frozen = false;
};

/** Downward expander. */
class NoiseGate
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setAmount (float amount01) noexcept;
    void process (float* x, int n) noexcept;

private:
    float envCoeffA = 0, envCoeffR = 0, gCoeffA = 0, gCoeffR = 0;
    float env = 0, gain = 1, threshDb = -90.0f;
    bool enabled = false;
};

/** Feed-forward compressor ("sustain") placed before the drive stage. */
class Compressor
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setAmount (float amount01) noexcept;
    void process (float* x, int n) noexcept;

private:
    float coeffA = 0, coeffR = 0, env = 0;
    float ratio = 1.0f, makeup = 1.0f;
    bool enabled = false;
};

/** The saturation stage: pre-shaping, one or two clipping stages at 4x
    oversampling, DC removal. */
class DriveStage
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void set (int voicing, float drive01) noexcept;

    /** In-place at base rate, n <= kMaxChunk. */
    void process (float* x, int n) noexcept;

    static float gainDbFor (int voicing, float drive01) noexcept;

private:
    struct Voice
    {
        float preCutHz, lowKeep, gainLoDb, gainHiDb, asym, bias;
        int stages;
        bool hard;
    };

    static const Voice& voice (int voicing) noexcept;

    double sr = 44100.0;
    Oversampler4x os;
    OnePole preLp, interHp, interLp, dcHp, postLp;
    int voicing = voicingCrunch;
    float gainTarget = 1.0f, gainCur = 1.0f;
    float work[kMaxChunk * Oversampler4x::kFactor] {};
};

/** Everything between the input jack and the matching EQ. Mono. */
class FrontEnd
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setDrive (int voicing, float drive01) noexcept { drive.set (voicing, drive01); }
    void setComp (float amount01) noexcept { comp.setAmount (amount01); }
    void setGate (float amount01) noexcept { gate.setAmount (amount01); }
    void setAdaptFrozen (bool frozen) noexcept { leveler.setFrozen (frozen); tilt.setFrozen (frozen); }
    void setCanonicalTilt (float tiltDb, bool enabled) noexcept { tilt.setTarget (tiltDb, enabled); }

    /** In-place, n <= kMaxChunk. Returns true if the chunk contained playing. */
    bool process (float* x, int n) noexcept;

    static constexpr int kLatency = Oversampler4x::kLatency;

    InputLeveler leveler;
    PickupTilt tilt;

private:
    OnePole inputHp;
    NoiseGate gate;
    Compressor comp;
    DriveStage drive;
};

} // namespace tg
