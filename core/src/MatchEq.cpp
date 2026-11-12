#include "tonegenie/MatchEq.h"

#include <cstring>

namespace tg
{

//==============================================================================
void MinPhaseDesigner::prepare (int numTapsToUse)
{
    taps = numTapsToUse;
    fft = std::make_unique<Fft> (taps * 4);
    buf.assign ((size_t) fft->size(), {});
}

void MinPhaseDesigner::design (const float* binDb, float* out)
{
    const int n = fft->size();
    const int half = n / 2;
    const float toLog = 0.11512925464970229f; // ln(10)/20

    for (int k = 0; k <= half; ++k)
    {
        const float lm = binDb[k] * toLog;
        buf[(size_t) k] = { lm, 0.0f };
        if (k > 0 && k < half)
            buf[(size_t) (n - k)] = { lm, 0.0f };
    }

    // Real cepstrum, folded to make it causal -> minimum phase.
    fft->inverse (buf.data());

    buf[0] = { buf[0].real(), 0.0f };
    for (int i = 1; i < half; ++i)
        buf[(size_t) i] = { 2.0f * buf[(size_t) i].real(), 0.0f };
    buf[(size_t) half] = { buf[(size_t) half].real(), 0.0f };
    for (int i = half + 1; i < n; ++i)
        buf[(size_t) i] = {};

    fft->forward (buf.data());

    for (int k = 0; k < n; ++k)
    {
        const float mag = std::exp (std::clamp (buf[(size_t) k].real(), -20.0f, 20.0f));
        const float ph = buf[(size_t) k].imag();
        buf[(size_t) k] = { mag * std::cos (ph), mag * std::sin (ph) };
    }

    fft->inverse (buf.data());

    const int fadeStart = taps - taps / 4;
    for (int i = 0; i < taps; ++i)
    {
        float w = 1.0f;
        if (i >= fadeStart)
            w = 0.5f + 0.5f * std::cos (kPi * (float) (i - fadeStart) / (float) (taps - fadeStart));
        out[i] = buf[(size_t) i].real() * w;
    }
}

//==============================================================================
namespace
{
    constexpr float kPriorSeconds = 1.5f;
    constexpr float kMemorySeconds = 20.0f;

    /** Generic guitar-speaker response used when no reference tone is loaded. */
    float defaultCurveDb (float hz) noexcept
    {
        float db = 0.0f;
        if (hz < 90.0f)
            db += 14.0f * std::log2 (hz / 90.0f);
        if (hz > 5000.0f)
            db -= 24.0f * std::log2 (hz / 5000.0f);

        const auto bump = [hz] (float fc, float widthOct, float gain)
        {
            const float d = std::log2 (hz / fc) / widthOct;
            return gain * std::exp (-0.5f * d * d);
        };

        db += bump (120.0f, 0.6f, 2.0f);
        db += bump (450.0f, 0.8f, -2.5f);
        db += bump (2800.0f, 0.6f, 4.0f);
        return db;
    }

    float nominalPsd (int band) noexcept
    {
        const float hz = bandCenterHz (band);
        const float bandPower = (hz >= 80.0f && hz <= 5000.0f) ? 0.02f / 36.0f : 0.0002f / 36.0f;
        return bandPower / bandWidthHz (band);
    }
}

MatchEq::MatchEq()
{
    for (int b = 0; b < kNumBands; ++b)
    {
        sharedMeasuredDb[(size_t) b].store (powerToDb (nominalPsd (b)));
        sharedTargetDb[(size_t) b].store (0.0f);
        sharedExpectedDb[(size_t) b].store (0.0f);
        sharedEqDb[(size_t) b].store (0.0f);
        avgPsd[(size_t) b] = nominalPsd (b);
    }
}

MatchEq::~MatchEq() = default;

void MatchEq::prepare (double sampleRate)
{
    sr = sampleRate;
    numTaps = sampleRate <= 50000.0 ? 1024 : (sampleRate <= 100000.0 ? 2048 : 4096);

    designer.prepare (numTaps);
    designMapper.prepare (sr, designer.fftSize());
    binDb.assign ((size_t) designer.fftSize() / 2 + 1, 0.0f);
    pendingFir.assign ((size_t) numTaps, 0.0f);

    fftSize = analysisFftSize (sr);
    hop = fftSize / 2;
    fft = std::make_unique<Fft> (fftSize);
    mapper.prepare (sr, fftSize);

    window.resize ((size_t) fftSize);
    double wsum = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * kPi * (float) i / (float) fftSize);
        wsum += (double) window[(size_t) i] * window[(size_t) i];
    }
    psdScale = (float) (2.0 / (sr * wsum));

    ring.assign ((size_t) fftSize, 0.0f);
    power.assign ((size_t) fftSize / 2 + 1, 0.0f);
    fftBuf.assign ((size_t) fftSize, {});

    maxWeight = (float) (kMemorySeconds * sr / hop);
    if (weight <= 0.0f)
        weight = (float) (kPriorSeconds * sr / hop);

    firCur.assign ((size_t) numTaps, 0.0f);
    firTarget.assign ((size_t) numTaps, 0.0f);
    history.assign ((size_t) (numTaps - 1 + kMaxChunk), 0.0f);
    scratch.assign ((size_t) kMaxChunk, 0.0f);

    reset();
    publishMeasured();

    // Start with a valid filter straight away.
    designNow();
}

void MatchEq::designNow()
{
    pendingState.store (0);
    needsDesign.store (true);
    service();
    std::memcpy (firTarget.data(), pendingFir.data(), sizeof (float) * (size_t) numTaps);
    firCur = firTarget;
    pendingState.store (0);
    smoothBlocks = 0;
}

void MatchEq::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (history.begin(), history.end(), 0.0f);
    ringPos = hopCount = activeCount = 0;
}

void MatchEq::setTarget (const BandArray& targetDb, const BandArray& expectedPreDb, float validLoHz, float validHiHz) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        sharedTargetDb[(size_t) b].store (targetDb[(size_t) b], std::memory_order_relaxed);
        sharedExpectedDb[(size_t) b].store (expectedPreDb[(size_t) b], std::memory_order_relaxed);
    }
    sharedValidLo.store (validLoHz);
    sharedValidHi.store (validHiHz);
    hasTarget.store (true);
    relearn();
}

void MatchEq::clearTarget() noexcept
{
    hasTarget.store (false);
    relearn();
}

void MatchEq::setAmount (float amount01) noexcept
{
    amount01 = std::clamp (amount01, 0.0f, 1.0f);
    if (std::abs (amount01 - sharedAmount.load (std::memory_order_relaxed)) > 1.0e-4f)
    {
        sharedAmount.store (amount01);
        needsDesign.store (true);
    }
}

void MatchEq::relearn() noexcept
{
    const bool target = hasTarget.load();
    for (int b = 0; b < kNumBands; ++b)
        avgPsd[(size_t) b] = target ? dbToPower (sharedExpectedDb[(size_t) b].load (std::memory_order_relaxed))
                                    : nominalPsd (b);

    weight = (float) (kPriorSeconds * sr / hop);
    publishMeasured();
}

void MatchEq::publishMeasured() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        sharedMeasuredDb[(size_t) b].store (powerToDb (avgPsd[(size_t) b]), std::memory_order_relaxed);
    sharedWeight.store (weight, std::memory_order_relaxed);
    needsDesign.store (true);
}

void MatchEq::getMeasured (BandArray& db, float& weightFrames) const noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        db[(size_t) b] = sharedMeasuredDb[(size_t) b].load (std::memory_order_relaxed);
    weightFrames = sharedWeight.load (std::memory_order_relaxed);
}

void MatchEq::setMeasured (const BandArray& db, float weightFrames) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        avgPsd[(size_t) b] = dbToPower (db[(size_t) b]);
    weight = std::clamp (weightFrames, 1.0f, maxWeight);
    publishMeasured();
}

void MatchEq::pushFrame() noexcept
{
    for (int i = 0; i < fftSize; ++i)
    {
        int idx = ringPos + i;
        if (idx >= fftSize)
            idx -= fftSize;
        fftBuf[(size_t) i] = { ring[(size_t) idx] * window[(size_t) i], 0.0f };
    }

    const bool frameActive = activeCount * 2 >= hop;
    activeCount = 0;
    hopCount = 0;

    if (! frameActive || ! adapting)
        return;

    fft->forward (fftBuf.data());
    for (int k = 0; k <= fftSize / 2; ++k)
        power[(size_t) k] = std::norm (fftBuf[(size_t) k]) * psdScale;

    BandArray frame;
    mapper.powerToBands (power.data(), frame.data());

    weight = std::min (weight + 1.0f, maxWeight);
    const float a = 1.0f / weight;
    for (int b = 0; b < kNumBands; ++b)
        avgPsd[(size_t) b] += a * (frame[(size_t) b] - avgPsd[(size_t) b]);

    publishMeasured();
}

void MatchEq::computeCurve (BandArray& eqDb, float& normGain) const noexcept
{
    const float amount = sharedAmount.load();
    const int lastBand = std::max (1, designMapper.numValidBands() - 1);

    BandArray measured;
    for (int b = 0; b < kNumBands; ++b)
        measured[(size_t) b] = sharedMeasuredDb[(size_t) b].load (std::memory_order_relaxed);

    if (hasTarget.load())
    {
        const int bLo = std::clamp ((int) std::ceil (bandIndexForHz (sharedValidLo.load())), 0, lastBand - 1);
        const int bHi = std::clamp ((int) std::floor (bandIndexForHz (sharedValidHi.load())), bLo + 1, lastBand);

        BandArray base {}, corr {};
        float corrMean = 0.0f;

        for (int b = bLo; b <= bHi; ++b)
        {
            const float target = sharedTargetDb[(size_t) b].load (std::memory_order_relaxed);
            const float expected = sharedExpectedDb[(size_t) b].load (std::memory_order_relaxed);
            base[(size_t) b] = target - expected;
            corr[(size_t) b] = expected - measured[(size_t) b];
            corrMean += corr[(size_t) b];
        }
        corrMean /= (float) (bHi - bLo + 1);

        // Outside the trusted range: hold the edge value, and keep rolling off above it
        // (nothing useful lives up there in a guitar tone, only fizz).
        for (int b = 0; b < bLo; ++b)
        {
            base[(size_t) b] = base[(size_t) bLo];
            corr[(size_t) b] = corr[(size_t) bLo];
        }
        for (int b = bHi + 1; b < kNumBands; ++b)
        {
            base[(size_t) b] = base[(size_t) bHi] - 3.0f * (float) (b - bHi);
            corr[(size_t) b] = corr[(size_t) bHi];
        }

        // The instrument correction is deliberately broad (2/3 octave) and bounded:
        // it should follow the guitar, not the notes being played.
        for (auto& c : corr)
            c -= corrMean;
        smoothBands (corr, corr, 4);

        float baseMean = 0.0f;
        for (int b = bLo; b <= bHi; ++b)
            baseMean += base[(size_t) b];
        baseMean /= (float) (bHi - bLo + 1);

        for (int b = 0; b < kNumBands; ++b)
        {
            const float c = std::clamp (corr[(size_t) b], -12.0f, 12.0f);
            const float v = std::clamp (base[(size_t) b] - baseMean, -48.0f, 30.0f);
            eqDb[(size_t) b] = amount * (v + c);
        }
    }
    else
    {
        for (int b = 0; b < kNumBands; ++b)
            eqDb[(size_t) b] = amount * defaultCurveDb (bandCenterHz (b));
    }

    // Loudness normalisation: make the predicted output power constant.
    double p = 0.0;
    for (int b = 0; b <= lastBand; ++b)
        p += (double) dbToPower (measured[(size_t) b] + eqDb[(size_t) b]) * bandWidthHz (b);

    normGain = (float) std::clamp (std::sqrt ((double) kOutputPower / std::max (p, 1.0e-12)), 0.01, 100.0);
}

bool MatchEq::service()
{
    if (! needsDesign.load() || pendingState.load() != 0)
        return false;

    // Only one thread designs at a time (the audio thread may also get here when rendering offline).
    if (designBusy.exchange (true))
        return false;

    struct Release
    {
        std::atomic<bool>& flag;
        ~Release() { flag.store (false); }
    } release { designBusy };

    needsDesign.store (false);

    BandArray eq;
    float norm = 1.0f;
    computeCurve (eq, norm);

    for (int b = 0; b < kNumBands; ++b)
        sharedEqDb[(size_t) b].store (eq[(size_t) b], std::memory_order_relaxed);

    designMapper.bandsToBins (eq.data(), binDb.data());
    designer.design (binDb.data(), pendingFir.data());

    for (auto& c : pendingFir)
        c *= norm;

    pendingState.store (1);
    return true;
}

void MatchEq::process (float* x, int n, bool active) noexcept
{
    // 1. measure what arrives from the drive stage
    for (int i = 0; i < n; ++i)
    {
        ring[(size_t) ringPos] = x[i];
        if (++ringPos >= fftSize)
            ringPos = 0;
        activeCount += active ? 1 : 0;

        if (++hopCount >= hop)
        {
            pushFrame();
            if (synchronous)
                service();
        }
    }

    // 2. pick up a freshly designed filter
    if (pendingState.load (std::memory_order_acquire) == 1)
    {
        std::memcpy (firTarget.data(), pendingFir.data(), sizeof (float) * (size_t) numTaps);
        pendingState.store (0, std::memory_order_release);
        smoothBlocks = (int) (0.4 * sr);
    }

    if (smoothBlocks > 0)
    {
        smoothBlocks -= n;
        if (smoothBlocks <= 0)
        {
            std::memcpy (firCur.data(), firTarget.data(), sizeof (float) * (size_t) numTaps);
        }
        else
        {
            const float a = 1.0f - std::exp (-(float) n / (float) (0.05 * sr));
            float* cur = firCur.data();
            const float* tgt = firTarget.data();
            for (int i = 0; i < numTaps; ++i)
                cur[i] += a * (tgt[i] - cur[i]);
        }
    }

    // 3. convolve
    float* hist = history.data();
    std::memcpy (hist + numTaps - 1, x, sizeof (float) * (size_t) n);

    float* out = scratch.data();
    std::memset (out, 0, sizeof (float) * (size_t) n);

    const float* h = firCur.data();
    for (int i = 0; i < numTaps; ++i)
    {
        const float c = h[i];
        const float* src = hist + (numTaps - 1 - i);
        for (int j = 0; j < n; ++j)
            out[j] += c * src[j];
    }

    std::memcpy (x, out, sizeof (float) * (size_t) n);
    std::memmove (hist, hist + n, sizeof (float) * (size_t) (numTaps - 1));
}

void MatchEq::getDisplay (BandArray& targetDb, BandArray& outputDb, BandArray& eqDb) const noexcept
{
    const bool target = hasTarget.load();
    const int lo = (int) std::lround (bandIndexForHz (150.0f));
    const int hi = (int) std::lround (bandIndexForHz (4000.0f));

    float tMean = 0.0f, oMean = 0.0f, eMean = 0.0f;

    for (int b = 0; b < kNumBands; ++b)
    {
        const float e = sharedEqDb[(size_t) b].load (std::memory_order_relaxed);
        const float m = sharedMeasuredDb[(size_t) b].load (std::memory_order_relaxed);
        eqDb[(size_t) b] = e;
        outputDb[(size_t) b] = m + e;
        targetDb[(size_t) b] = target ? sharedTargetDb[(size_t) b].load (std::memory_order_relaxed) : m + e;

        if (b >= lo && b <= hi)
        {
            tMean += targetDb[(size_t) b];
            oMean += outputDb[(size_t) b];
            eMean += e;
        }
    }

    const float cnt = (float) (hi - lo + 1);
    for (int b = 0; b < kNumBands; ++b)
    {
        targetDb[(size_t) b] -= tMean / cnt;
        outputDb[(size_t) b] -= oMean / cnt;
        eqDb[(size_t) b] -= eMean / cnt;
    }
}

} // namespace tg
