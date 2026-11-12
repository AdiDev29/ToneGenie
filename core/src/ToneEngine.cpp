#include "tonegenie/ToneEngine.h"

namespace tg
{

//==============================================================================
void EchoDelay::prepare (double sampleRate)
{
    sr = sampleRate;
    line.prepare ((int) (2.0 * sr) + 8);
    damp.setCutoff (4500.0, sr);
    timeCoeff = onePoleCoeff (0.15, sr);
    timeCur = timeTarget;
    mixCur = mixTarget;
    reset();
}

void EchoDelay::reset() noexcept
{
    line.reset();
    damp.reset();
}

void EchoDelay::set (float timeMs, float feedback, float mix) noexcept
{
    timeTarget = std::clamp ((float) (timeMs * 0.001 * sr), 2.0f, (float) (1.9 * sr));
    fb = std::clamp (feedback, 0.0f, 0.92f);
    mixTarget = std::clamp (mix, 0.0f, 1.0f);
}

void EchoDelay::process (float* x, int n) noexcept
{
    const float mixStep = (mixTarget - mixCur) / (float) n;

    for (int i = 0; i < n; ++i)
    {
        timeCur += timeCoeff * (timeTarget - timeCur);
        mixCur += mixStep;

        const float d = line.readInterp (timeCur);
        line.push (x[i] + fb * damp.lp (d));
        x[i] += mixCur * d;
    }

    mixCur = mixTarget;
}

//==============================================================================
void Reverb::prepare (double sampleRate)
{
    sr = sampleRate;

    static const double ms[kLines] = { 29.7, 37.1, 41.1, 43.7, 47.9, 53.3, 59.9, 67.7 };

    for (int i = 0; i < kLines; ++i)
    {
        lineLen[i] = ((int) (ms[i] * 0.001 * sr)) | 1;
        lines[i].prepare (lineLen[i] + 2);
        lineDamp[i].setCutoff (4500.0, sr);
    }

    preLen = std::max (1, (int) (0.012 * sr));
    ap1Len = std::max (1, (int) (0.0051 * sr)) | 1;
    ap2Len = std::max (1, (int) (0.0126 * sr)) | 1;
    pre.prepare (preLen + 2);
    ap1.prepare (ap1Len + 2);
    ap2.prepare (ap2Len + 2);
    inHp.setCutoff (160.0, sr);

    const float d = decay > 0.0f ? decay : 1.2f;
    decay = -1.0f;
    set (mixTarget, d);
    mixCur = mixTarget;
    reset();
}

void Reverb::reset() noexcept
{
    pre.reset();
    ap1.reset();
    ap2.reset();
    inHp.reset();
    for (int i = 0; i < kLines; ++i)
    {
        lines[i].reset();
        lineDamp[i].reset();
    }
}

void Reverb::set (float mix, float decaySeconds) noexcept
{
    mixTarget = std::clamp (mix, 0.0f, 1.0f);
    decaySeconds = std::clamp (decaySeconds, 0.2f, 8.0f);

    if (std::abs (decaySeconds - decay) > 0.005f)
    {
        decay = decaySeconds;
        for (int i = 0; i < kLines; ++i)
            lineGain[i] = std::pow (10.0f, -3.0f * (float) lineLen[i] / (decay * (float) sr));
    }
}

void Reverb::process (const float* in, float* outL, float* outR, int n) noexcept
{
    if (mixTarget <= 0.0f && mixCur <= 0.0f)
    {
        if (! idle)
        {
            reset(); // drop the tail so it cannot come back when the reverb is turned up again
            idle = true;
        }

        for (int i = 0; i < n; ++i)
            outL[i] = outR[i] = in[i];
        return;
    }

    idle = false;

    const float mixStep = (mixTarget - mixCur) / (float) n;
    // Keeps the reverb level roughly independent of the decay time.
    const float g = lineGain[kLines / 2];
    const float level = 0.55f * std::sqrt (std::max (0.02f, 1.0f - g * g));
    constexpr float had = 0.35355339f; // 1/sqrt(8)

    for (int s = 0; s < n; ++s)
    {
        mixCur += mixStep;

        float v = inHp.hp (in[s]);
        pre.push (v);
        v = pre.read (preLen);

        {
            const float d = ap1.read (ap1Len);
            const float w = v + 0.6f * d;
            ap1.push (w);
            v = d - 0.6f * w;
        }
        {
            const float d = ap2.read (ap2Len);
            const float w = v + 0.6f * d;
            ap2.push (w);
            v = d - 0.6f * w;
        }

        float o[kLines];
        for (int i = 0; i < kLines; ++i)
            o[i] = lineDamp[i].lp (lines[i].read (lineLen[i])) * lineGain[i];

        const float wetL = o[0] - o[1] + o[2] - o[3] + o[4] - o[5] + o[6] - o[7];
        const float wetR = o[0] + o[1] - o[2] - o[3] + o[4] + o[5] - o[6] - o[7];

        // Fast Walsh-Hadamard mixing matrix.
        for (int h = 1; h < kLines; h <<= 1)
            for (int i = 0; i < kLines; i += h << 1)
                for (int j = i; j < i + h; ++j)
                {
                    const float a = o[j], b = o[j + h];
                    o[j] = a + b;
                    o[j + h] = a - b;
                }

        for (int i = 0; i < kLines; ++i)
            lines[i].push (v + o[i] * had);

        const float dry = in[s] * (1.0f - 0.5f * mixCur);
        outL[s] = dry + mixCur * level * wetL;
        outR[s] = dry + mixCur * level * wetR;
    }

    mixCur = mixTarget;
}

//==============================================================================
void ToneEngine::prepare (double sampleRate)
{
    sr = sampleRate;
    front.prepare (sr);
    eq.prepare (sr);
    delay.prepare (sr);
    reverb.prepare (sr);
    dryL.prepare (latencySamples() + kMaxChunk);
    dryR.prepare (latencySamples() + kMaxChunk);
    setParams (params);
    delay.snap();
    front.snapSmoothers();
    mixCur = params.mix;
    outCur = dbToGain (params.outputDb);
}

void ToneEngine::reset() noexcept
{
    front.reset();
    eq.reset();
    delay.reset();
    reverb.reset();
    dryL.reset();
    dryR.reset();
}

void ToneEngine::setParams (const EngineParams& p) noexcept
{
    params = p;
    front.setDrive (p.voicing, p.drive);
    front.setComp (p.comp);
    front.setGate (p.gate);
    front.setAdaptFrozen (! p.adapt);
    eq.setAmount (p.match);
    eq.setAdapting (p.adapt);
    delay.set (p.delayTimeMs, p.delayFeedback, p.delayMix);
    reverb.set (p.reverbMix, p.reverbDecay);
}

void ToneEngine::setProfile (const ToneProfile* profile) noexcept
{
    if (profile != nullptr && profile->valid)
    {
        eq.setTarget (profile->targetDb, profile->expectedPreDb, profile->validLoHz, profile->validHiHz);
        front.setCanonicalTilt (profile->canonicalTiltDb, true);
    }
    else
    {
        eq.clearTarget();
        front.setCanonicalTilt (0.0f, false);
    }
}

void ToneEngine::relearn() noexcept
{
    eq.relearn();
    front.leveler.setState (-26.0f, 0.0f);
    front.tilt.setState (0.0f, 0.0f, 0.0f);
}

void ToneEngine::setAdaptState (const AdaptState& s) noexcept
{
    if (! s.valid)
        return;

    front.leveler.setState (s.levelDb, s.levelSeconds);
    front.tilt.setState (s.tiltLow, s.tiltHigh, s.tiltSeconds);
    eq.setMeasured (s.measuredDb, s.measuredWeight);
}

AdaptState ToneEngine::getAdaptState() const noexcept
{
    AdaptState s;
    s.valid = true;
    s.levelDb = stLevel.load (std::memory_order_relaxed);
    s.levelSeconds = stLevelSecs.load (std::memory_order_relaxed);
    s.tiltLow = stTiltLow.load (std::memory_order_relaxed);
    s.tiltHigh = stTiltHigh.load (std::memory_order_relaxed);
    s.tiltSeconds = stTiltSecs.load (std::memory_order_relaxed);
    eq.getMeasured (s.measuredDb, s.measuredWeight);
    return s;
}

void ToneEngine::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const int lat = latencySamples();
    const float outTarget = dbToGain (params.outputDb);
    const float mixTarget = std::clamp (params.mix, 0.0f, 1.0f);

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min (kMaxChunk, numSamples - pos);

        float mono[kMaxChunk], dL[kMaxChunk], dR[kMaxChunk], wL[kMaxChunk], wR[kMaxChunk];

        for (int i = 0; i < n; ++i)
        {
            // One NaN from an upstream plugin must not poison the filters for good.
            const auto clean = [] (float v) noexcept { return std::isfinite (v) ? std::clamp (v, -64.0f, 64.0f) : 0.0f; };
            const float l = clean (inL[pos + i]);
            const float r = inR != nullptr ? clean (inR[pos + i]) : l;
            mono[i] = 0.5f * (l + r);

            // latency-compensated dry path
            dryL.push (l);
            dryR.push (r);
            dL[i] = dryL.read (lat + 1);
            dR[i] = dryR.read (lat + 1);
        }

        const bool active = front.process (mono, n);
        eq.process (mono, n, active);
        delay.process (mono, n);
        reverb.process (mono, wL, wR, n);

        const float mixStep = (mixTarget - mixCur) / (float) n;
        const float outStep = (outTarget - outCur) / (float) n;

        for (int i = 0; i < n; ++i)
        {
            mixCur += mixStep;
            outCur += outStep;

            const float l = (dL[i] + mixCur * (wL[i] - dL[i])) * outCur;
            const float r = (dR[i] + mixCur * (wR[i] - dR[i])) * outCur;

            if (outR != nullptr)
            {
                outL[pos + i] = l;
                outR[pos + i] = r;
            }
            else
            {
                outL[pos + i] = 0.5f * (l + r);
            }
        }

        mixCur = mixTarget;
        outCur = outTarget;

        meterActive.store (active, std::memory_order_relaxed);
        pos += n;
    }

    meterInputGain.store (front.leveler.gainDb(), std::memory_order_relaxed);
    meterTilt.store (front.tilt.correctionDb(), std::memory_order_relaxed);
    stLevel.store (front.leveler.trackedDb(), std::memory_order_relaxed);
    stLevelSecs.store (front.leveler.activeSeconds(), std::memory_order_relaxed);

    float lo, hi, secs;
    front.tilt.getState (lo, hi, secs);
    stTiltLow.store (lo, std::memory_order_relaxed);
    stTiltHigh.store (hi, std::memory_order_relaxed);
    stTiltSecs.store (secs, std::memory_order_relaxed);
}

} // namespace tg
