#include "tonegenie/FrontEnd.h"

#include <cstring>

namespace tg
{

//==============================================================================
static double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1.0e-12 * sum)
            break;
    }
    return sum;
}

Oversampler4x::Oversampler4x()
{
    // Kaiser-windowed sinc, cutoff at the base-rate Nyquist, ~80 dB stop-band.
    const double fc = 0.5 / kFactor;
    const double beta = 7.86;
    const int mid = (kTaps - 1) / 2;
    double sum = 0.0;
    double proto[kTaps];

    for (int i = 0; i < kTaps; ++i)
    {
        const double t = i - mid;
        const double sinc = t == 0.0 ? 2.0 * fc : std::sin (2.0 * 3.14159265358979323846 * fc * t) / (3.14159265358979323846 * t);
        const double r = t / mid;
        const double win = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
        proto[i] = sinc * win;
        sum += proto[i];
    }

    for (int i = 0; i < kTaps; ++i)
        h[i] = (float) (proto[i] / sum);

    for (int p = 0; p < kFactor; ++p)
        for (int k = 0; k < kPhaseTaps; ++k)
        {
            const int idx = kFactor * k + p;
            phase[p][k] = idx < kTaps ? h[idx] * (float) kFactor : 0.0f;
        }

    reset();
}

void Oversampler4x::reset() noexcept
{
    std::memset (upBuf, 0, sizeof (upBuf));
    std::memset (downBuf, 0, sizeof (downBuf));
}

void Oversampler4x::up (const float* in, float* out, int n) noexcept
{
    constexpr int hist = kPhaseTaps - 1;
    std::memcpy (upBuf + hist, in, sizeof (float) * (size_t) n);

    for (int j = 0; j < n; ++j)
    {
        const float* x = upBuf + hist + j;
        for (int p = 0; p < kFactor; ++p)
        {
            const float* c = phase[p];
            float acc = 0.0f;
            for (int k = 0; k < kPhaseTaps; ++k)
                acc += c[k] * x[-k];
            out[kFactor * j + p] = acc;
        }
    }

    std::memmove (upBuf, upBuf + n, sizeof (float) * (size_t) hist);
}

void Oversampler4x::down (const float* in, float* out, int n) noexcept
{
    constexpr int hist = kTaps - 1;
    std::memcpy (downBuf + hist, in, sizeof (float) * (size_t) (kFactor * n));

    for (int j = 0; j < n; ++j)
    {
        const float* x = downBuf + hist + kFactor * j;
        float acc = 0.0f;
        for (int i = 0; i < kTaps; ++i)
            acc += h[i] * x[-i];
        out[j] = acc;
    }

    std::memmove (downBuf, downBuf + kFactor * n, sizeof (float) * (size_t) hist);
}

//==============================================================================
void InputLeveler::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    msCoeff = onePoleCoeff (0.015, sampleRate);
    reset();
}

void InputLeveler::reset() noexcept
{
    ms = 0.0f;
    gain = dbToGain (targetGainDb);
}

void InputLeveler::setState (float trackedLevelDb, float activeSecs) noexcept
{
    level = std::clamp (trackedLevelDb, -70.0f, 6.0f);
    activeTime = std::max (0.0f, activeSecs);
    targetGainDb = std::clamp (kReferenceDb - level, -24.0f, 40.0f);
    gain = dbToGain (targetGainDb);
}

bool InputLeveler::process (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        ms += msCoeff * (x[i] * x[i] - ms);

    const float rmsDb = powerToDb (ms);
    const bool active = rmsDb > std::max (-65.0f, level - 30.0f);

    if (active && ! frozen)
    {
        const float dt = (float) (n / sr);
        const float u = std::clamp (activeTime / 3.0f, 0.0f, 1.0f);
        // Fast while learning a new instrument, then slow enough to leave playing dynamics alone.
        const float tau = rmsDb > level ? 0.03f + u * 0.37f : 0.3f + u * 11.7f;
        level += (1.0f - std::exp (-dt / tau)) * (rmsDb - level);
        activeTime += dt;
    }

    targetGainDb = std::clamp (kReferenceDb - level, -24.0f, 40.0f);
    const float target = dbToGain (targetGainDb);
    const float c = onePoleCoeff (0.03, sr);

    for (int i = 0; i < n; ++i)
    {
        gain += c * (target - gain);
        x[i] *= gain;
    }

    return active;
}

//==============================================================================
void PickupTilt::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    bpLow.setBandPass (200.0, 0.7, sr);
    bpHigh.setBandPass (2200.0, 0.7, sr);
    msCoeff = onePoleCoeff (0.05, sr);
    appliedDb = 1000.0f; // force filter update
    reset();
    updateFilter();
}

void PickupTilt::reset() noexcept
{
    bpLow.reset();
    bpHigh.reset();
    shelf.reset();
    msLow = msHigh = 0.0f;
}

float PickupTilt::measuredDb() const noexcept
{
    return powerToDb (avgHigh) - powerToDb (avgLow);
}

void PickupTilt::setState (float lowPower, float highPower, float weightSecs) noexcept
{
    avgLow = std::max (0.0f, lowPower);
    avgHigh = std::max (0.0f, highPower);
    weight = std::max (0.0f, weightSecs);
}

void PickupTilt::getState (float& lowPower, float& highPower, float& weightSecs) const noexcept
{
    lowPower = avgLow;
    highPower = avgHigh;
    weightSecs = weight;
}

void PickupTilt::updateFilter() noexcept
{
    const float wanted = (on && weight > 0.3f) ? std::clamp ((target - measuredDb()) * 0.85f, -9.0f, 9.0f) : 0.0f;
    corrDb = wanted;

    if (std::abs (wanted - appliedDb) > 0.1f)
    {
        appliedDb = wanted;
        shelf.setHighShelf (700.0, appliedDb, sr);
        broadband = dbToGain (-0.5f * appliedDb);
    }
}

void PickupTilt::process (float* x, int n, bool active) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float lo = bpLow.process (x[i]);
        const float hi = bpHigh.process (x[i]);
        msLow += msCoeff * (lo * lo - msLow);
        msHigh += msCoeff * (hi * hi - msHigh);
        x[i] = shelf.process (x[i]) * broadband;
    }

    if (active && ! frozen)
    {
        const float dt = (float) (n / sr);
        weight = std::min (weight + dt, 10.0f);
        const float a = dt / weight;
        avgLow += a * (msLow - avgLow);
        avgHigh += a * (msHigh - avgHigh);
    }

    updateFilter();
}

//==============================================================================
void NoiseGate::prepare (double sampleRate) noexcept
{
    envCoeffA = onePoleCoeff (0.001, sampleRate);
    envCoeffR = onePoleCoeff (0.08, sampleRate);
    gCoeffA = onePoleCoeff (0.002, sampleRate);
    gCoeffR = onePoleCoeff (0.06, sampleRate);
    reset();
}

void NoiseGate::reset() noexcept
{
    env = 0.0f;
    gain = 1.0f;
}

void NoiseGate::setAmount (float amount01) noexcept
{
    enabled = amount01 > 0.005f;
    threshDb = -75.0f + 40.0f * std::clamp (amount01, 0.0f, 1.0f);
}

void NoiseGate::process (float* x, int n) noexcept
{
    if (! enabled)
    {
        gain = 1.0f;
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (x[i]);
        env += (a > env ? envCoeffA : envCoeffR) * (a - env);

        const float envDb = gainToDb (env);
        const float target = envDb >= threshDb ? 1.0f : dbToGain (std::max (-70.0f, -2.0f * (threshDb - envDb)));
        gain += (target > gain ? gCoeffA : gCoeffR) * (target - gain);
        x[i] *= gain;
    }
}

//==============================================================================
void Compressor::prepare (double sampleRate) noexcept
{
    coeffA = onePoleCoeff (0.005, sampleRate);
    coeffR = onePoleCoeff (0.15, sampleRate);
    reset();
}

void Compressor::reset() noexcept { env = 0.0f; }

void Compressor::setAmount (float amount01) noexcept
{
    const float a = std::clamp (amount01, 0.0f, 1.0f);
    enabled = a > 0.005f;
    ratio = 1.0f + 7.0f * a;
    makeup = dbToGain (20.0f * (1.0f - 1.0f / ratio));
}

void Compressor::process (float* x, int n) noexcept
{
    if (! enabled)
        return;

    constexpr float threshDb = -34.0f;
    const float slope = 1.0f - 1.0f / ratio;

    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (x[i]);
        env += (a > env ? coeffA : coeffR) * (a - env);

        const float over = gainToDb (env) - threshDb;
        const float g = over > 0.0f ? dbToGain (-over * slope) : 1.0f;
        x[i] *= g * makeup;
    }
}

//==============================================================================
const DriveStage::Voice& DriveStage::voice (int v) noexcept
{
    //                          preCut lowKeep  gLo   gHi   asym  bias stages hard
    static const Voice voices[numVoicings] = {
        /* Smooth */ { 350.0f, 0.35f, -6.0f, 34.0f, 1.2f, 0.0f, 1, false },
        /* Crunch */ { 110.0f, 0.30f, -6.0f, 52.0f, 1.4f, 0.0f, 2, false },
        /* Fuzz   */ { 60.0f, 0.80f, 12.0f, 62.0f, 1.0f, 0.3f, 2, true },
    };
    return voices[std::clamp (v, 0, (int) numVoicings - 1)];
}

float DriveStage::gainDbFor (int v, float drive01) noexcept
{
    const auto& vc = voice (v);
    return vc.gainLoDb + std::clamp (drive01, 0.0f, 1.0f) * (vc.gainHiDb - vc.gainLoDb);
}

void DriveStage::prepare (double sampleRate) noexcept
{
    sr = sampleRate;
    interHp.setCutoff (70.0, sr * Oversampler4x::kFactor);
    interLp.setCutoff (6500.0, sr * Oversampler4x::kFactor);
    dcHp.setCutoff (25.0, sr);
    postLp.setCutoff (11000.0, sr);
    preLp.setCutoff (voice (voicing).preCutHz, sr);
    gainCur = gainTarget;
    reset();
}

void DriveStage::reset() noexcept
{
    os.reset();
    preLp.reset();
    interHp.reset();
    interLp.reset();
    dcHp.reset();
    postLp.reset();
}

void DriveStage::set (int newVoicing, float drive01) noexcept
{
    newVoicing = std::clamp (newVoicing, 0, (int) numVoicings - 1);
    if (newVoicing != voicing)
    {
        voicing = newVoicing;
        preLp.setCutoff (voice (voicing).preCutHz, sr);
    }
    gainTarget = dbToGain (gainDbFor (voicing, drive01));
}

static inline float softClip (float x, float asym) noexcept
{
    return x >= 0.0f ? std::tanh (x) : std::tanh (asym * x) / asym;
}

static inline float hardClip (float x) noexcept
{
    return x / std::sqrt (std::sqrt (1.0f + x * x * x * x));
}

void DriveStage::process (float* x, int n) noexcept
{
    const auto& vc = voice (voicing);
    const float lowCut = 1.0f - vc.lowKeep;

    for (int i = 0; i < n; ++i)
        x[i] -= lowCut * preLp.lp (x[i]);

    os.up (x, work, n);

    const int m = n * Oversampler4x::kFactor;
    const float gEnd = gainCur + 0.25f * (gainTarget - gainCur);

    if (vc.stages == 1)
    {
        const float gStep = (gEnd - gainCur) / (float) m;
        float g = gainCur;

        for (int i = 0; i < m; ++i)
        {
            g += gStep;
            work[i] = softClip (work[i] * g, vc.asym);
        }
    }
    else
    {
        // Two cascaded stages share the gain: slightly more in the first.
        const float a0 = std::pow (gainCur, 0.55f), a1 = std::pow (gEnd, 0.55f);
        const float b0 = 1.6f * gainCur / a0, b1 = 1.6f * gEnd / a1;
        const float aStep = (a1 - a0) / (float) m, bStep = (b1 - b0) / (float) m;
        float g1 = a0, g2 = b0;

        if (! vc.hard)
        {
            for (int i = 0; i < m; ++i)
            {
                g1 += aStep;
                g2 += bStep;
                float v = softClip (work[i] * g1, vc.asym);
                v = interLp.lp (interHp.hp (v));
                work[i] = std::tanh (v * g2);
            }
        }
        else
        {
            const float biasOut = hardClip (vc.bias);
            for (int i = 0; i < m; ++i)
            {
                g1 += aStep;
                g2 += bStep;
                float v = hardClip (work[i] * g1 + vc.bias) - biasOut;
                v = interLp.lp (interHp.hp (v));
                work[i] = hardClip (v * g2);
            }
        }
    }

    gainCur = gEnd;

    os.down (work, x, n);

    for (int i = 0; i < n; ++i)
        x[i] = 0.3f * postLp.lp (dcHp.hp (x[i]));
}

//==============================================================================
void FrontEnd::prepare (double sampleRate) noexcept
{
    inputHp.setCutoff (30.0, sampleRate);
    leveler.prepare (sampleRate);
    tilt.prepare (sampleRate);
    gate.prepare (sampleRate);
    comp.prepare (sampleRate);
    drive.prepare (sampleRate);
    reset();
}

void FrontEnd::reset() noexcept
{
    inputHp.reset();
    leveler.reset();
    tilt.reset();
    gate.reset();
    comp.reset();
    drive.reset();
}

bool FrontEnd::process (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        x[i] = inputHp.hp (x[i]);

    const bool active = leveler.process (x, n);
    tilt.process (x, n, active);
    gate.process (x, n);
    comp.process (x, n);
    drive.process (x, n);
    return active;
}

} // namespace tg
