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

    Threading: process() and the setters run on the audio thread and do
    nothing but buffer, convolve and flip atomics. All spectral analysis and
    filter design happens in service(), which runs on a helper thread (or
    inside process() in synchronous mode, for offline rendering and tests). */
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

    /** Restores a previously learned instrument spectrum. */
    void setMeasured (const BandArray& db, float weightFrames) noexcept;

    /** When true the analysis and filter design run inside process(). */
    void setSynchronous (bool sync) noexcept { synchronous = sync; }

    /** In-place, n <= kMaxChunk. `active` says whether the player is playing. */
    void process (float* x, int n, bool active) noexcept;

    /** Non-realtime work: analyses a waiting frame, redesigns the filter if
        needed. Returns true if a new filter was designed. Safe to call from
        any one thread at a time (concurrent calls simply return). */
    bool service();

    /** Designs and installs the filter immediately, without cross-fading.
        Only for use while process() is not running (setup, offline fitting). */
    void designNow();

    // --- state / display (any thread) ----------------------------------------
    void getMeasured (BandArray& db, float& weightFrames) const noexcept;
    void getDisplay (BandArray& targetDb, BandArray& outputDb, BandArray& eqDb) const noexcept;

    static constexpr float kOutputPower = 0.01f; ///< -20 dBFS RMS while playing

private:
    enum Command { cmdNone = 0, cmdRelearn = 1, cmdRestore = 2 };

    void handleCommand (int cmd) noexcept;
    void analyseFrame() noexcept;
    void publishMeasured() noexcept;
    void computeCurve (BandArray& eqDb, float& normGain) const noexcept;
    void installPending() noexcept;

    double sr = 44100.0;
    int numTaps = 1024;
    int fftSize = 4096, hop = 2048;
    float priorWeight = 30.0f, maxWeight = 400.0f;

    // --- audio thread -------------------------------------------------------------
    std::vector<float> ring;
    int ringPos = 0, hopCount = 0, activeCount = 0;
    bool adapting = true, synchronous = false;
    std::vector<float> firCur, firTarget;   ///< stored reversed, see process()
    std::vector<float> history;
    int smoothSamplesLeft = 0;

    // --- hand-over between the threads ----------------------------------------------
    std::vector<float> frameBuf;             ///< audio -> design, guarded by frameState
    std::atomic<int> frameState { 0 };       ///< 0 free, 1 holds a frame
    std::vector<float> pendingFir;           ///< design -> audio, guarded by pendingState
    std::atomic<int> pendingState { 0 };     ///< 0 free, 1 holds a filter
    std::atomic<int> command { cmdNone };
    std::atomic<bool> needsDesign { true };
    std::atomic<bool> designBusy { false };
    std::atomic<bool> hasTarget { false };

    std::array<std::atomic<float>, kNumBands> sharedMeasuredDb, sharedRestoreDb, sharedTargetDb, sharedExpectedDb, sharedEqDb;
    std::atomic<float> sharedWeight { 0.0f }, sharedRestoreWeight { 0.0f };
    std::atomic<float> sharedAmount { 1.0f }, sharedValidLo { 60.0f }, sharedValidHi { 16000.0f };

    // --- design thread ----------------------------------------------------------------
    std::unique_ptr<Fft> fft;
    BandMapper mapper, designMapper;
    MinPhaseDesigner designer;
    std::vector<float> window, power, binDb;
    std::vector<std::complex<float>> fftBuf;
    float psdScale = 1.0f;
    BandArray avgPsd {};
    float weight = 0.0f;
};

} // namespace tg
