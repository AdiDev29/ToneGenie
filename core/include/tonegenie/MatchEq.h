#pragma once

#include "Bands.h"
#include "DspBlocks.h"
#include "Fft.h"

#include <atomic>
#include <complex>
#include <memory>
#include <vector>

namespace tg
{

/** Designs minimum-phase FIR filters from a magnitude response (cepstral method).
    Minimum phase = no pre-ringing and no added latency, like a real amp/cab. */
class MinPhaseDesigner
{
public:
    void prepare (int numTaps);

    /** binDb: (fftSize/2 + 1) gains in dB. Writes numTaps coefficients. */
    void design (const float* binDb, float* taps);

    int fftSize() const noexcept { return fft ? fft->size() : 0; }
    int numTaps() const noexcept { return taps; }

private:
    std::unique_ptr<Fft> fft;
    std::vector<std::complex<float>> buf;
    int taps = 0;
};

/** The adaptive tone-matching equaliser.

    It continuously measures the long-term spectrum arriving from the drive
    stage and shapes it into the target spectrum of the reference tone. The
    fine structure of the curve (the "amp + cabinet" part) comes from the
    profile; a broad, slowly adapting correction on top compensates for the
    instrument actually being played.

    Threading: process() and the setters run on the audio thread. service()
    does the heavy filter design and may run on any other thread; the two
    sides only exchange data through atomics. */
class MatchEq
{
public:
    MatchEq();
    ~MatchEq();

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Sets the target tone. expectedPreDb is the spectrum the drive stage is
        expected to deliver before any live measurement exists. */
    void setTarget (const BandArray& targetDb, const BandArray& expectedPreDb, float validLoHz, float validHiHz) noexcept;

    /** Removes the target: a fixed generic speaker-like curve is used instead. */
    void clearTarget() noexcept;

    void setAmount (float amount01) noexcept;
    void setAdapting (bool shouldAdapt) noexcept { adapting = shouldAdapt; }

    /** Forget what was learned about the instrument and start again. */
    void relearn() noexcept;

    /** When true the filter design runs inside process() (offline rendering, tests). */
    void setSynchronous (bool sync) noexcept { synchronous = sync; }

    /** In-place, n <= kMaxChunk. `active` says whether the player is playing. */
    void process (float* x, int n, bool active) noexcept;

    /** Non-realtime work. Returns true if a new filter was designed. */
    bool service();

    /** Designs and installs the filter immediately, without cross-fading.
        Only for use while process() is not running (setup, offline fitting). */
    void designNow();

    // --- state / display (any thread) ----------------------------------------
    void getMeasured (BandArray& db, float& weightFrames) const noexcept;
    void setMeasured (const BandArray& db, float weightFrames) noexcept; // audio thread
    void getDisplay (BandArray& targetDb, BandArray& outputDb, BandArray& eqDb) const noexcept;

    static constexpr float kOutputPower = 0.01f; ///< -20 dBFS RMS while playing

private:
    void pushFrame() noexcept;
    void publishMeasured() noexcept;
    void computeCurve (BandArray& eqDb, float& normGain) const noexcept;

    double sr = 44100.0;
    int numTaps = 1024;

    // analysis
    int fftSize = 4096, hop = 2048;
    std::unique_ptr<Fft> fft;
    BandMapper mapper, designMapper;
    std::vector<float> window, ring, power;
    std::vector<std::complex<float>> fftBuf;
    int ringPos = 0, hopCount = 0, activeCount = 0;
    float psdScale = 1.0f;

    BandArray avgPsd {};
    float weight = 0.0f, maxWeight = 400.0f;
    bool adapting = true, synchronous = false;

    // shared with the design thread
    std::array<std::atomic<float>, kNumBands> sharedMeasuredDb, sharedTargetDb, sharedExpectedDb, sharedEqDb;
    std::atomic<float> sharedWeight { 0.0f }, sharedAmount { 1.0f }, sharedValidLo { 60.0f }, sharedValidHi { 16000.0f };
    std::atomic<bool> hasTarget { false };
    std::atomic<bool> needsDesign { true };
    std::atomic<bool> designBusy { false };
    std::atomic<int> pendingState { 0 };

    // design side
    MinPhaseDesigner designer;
    std::vector<float> binDb, pendingFir;

    // filter
    std::vector<float> firCur, firTarget, history, scratch;
    int smoothBlocks = 0;
    float smoothCoeff = 0.1f;
};

} // namespace tg
