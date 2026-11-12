#include "tonegenie/Analysis.h"
#include "tonegenie/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>

namespace tg
{

float percentile (std::vector<float> values, float p)
{
    if (values.empty())
        return 0.0f;

    const auto idx = (size_t) std::clamp ((double) p * (double) (values.size() - 1), 0.0, (double) (values.size() - 1));
    std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) idx, values.end());
    return values[idx];
}

FrameAnalysis analyzeFrames (const float* x, size_t numSamples, double sampleRate)
{
    FrameAnalysis out;
    out.sampleRate = sampleRate;
    out.fftSize = analysisFftSize (sampleRate);
    out.hop = out.fftSize / 2;

    const int n = out.fftSize;
    if (numSamples < (size_t) n)
        return out;

    out.numFrames = (int) ((numSamples - (size_t) n) / (size_t) out.hop) + 1;

    Fft fft (n);
    BandMapper mapper;
    mapper.prepare (sampleRate, n);

    std::vector<float> window ((size_t) n);
    double wsum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * 3.14159265f * (float) i / (float) n);
        wsum += (double) window[(size_t) i] * window[(size_t) i];
    }

    const float psdScale = (float) (2.0 / (sampleRate * wsum));
    std::vector<std::complex<float>> buf ((size_t) n);
    std::vector<float> power ((size_t) n / 2 + 1);

    const double binHz = sampleRate / n;
    const int denLo = std::max (1, (int) std::lround (150.0 / binHz));
    const int denHi = std::min (n / 2 - 1, (int) std::lround (4000.0 / binHz));
    std::vector<float> sorted ((size_t) (denHi - denLo + 1));

    out.bandPsd.resize ((size_t) out.numFrames * kNumBands);
    out.rmsDb.resize ((size_t) out.numFrames);
    out.crestDb.resize ((size_t) out.numFrames);
    out.density.resize ((size_t) out.numFrames);

    for (int f = 0; f < out.numFrames; ++f)
    {
        const float* src = x + (size_t) f * (size_t) out.hop;

        double sq = 0.0;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float v = src[i];
            sq += (double) v * v;
            peak = std::max (peak, std::abs (v));
            buf[(size_t) i] = { v * window[(size_t) i], 0.0f };
        }

        const float ms = (float) (sq / n);
        out.rmsDb[(size_t) f] = powerToDb (ms);
        out.crestDb[(size_t) f] = powerToDb (peak * peak) - powerToDb (ms);

        fft.forward (buf.data());

        for (int k = 0; k <= n / 2; ++k)
            power[(size_t) k] = std::norm (buf[(size_t) k]) * psdScale;

        mapper.powerToBands (power.data(), &out.bandPsd[(size_t) f * kNumBands]);

        // Spectral density: how many bins are needed to hold 85% of the energy.
        std::copy (power.begin() + denLo, power.begin() + denHi + 1, sorted.begin());
        std::sort (sorted.begin(), sorted.end(), std::greater<float>());
        double bandTotal = 0.0;
        for (float p : sorted)
            bandTotal += p;
        double run = 0.0;
        int needed = 0;
        while (needed < (int) sorted.size() && run < 0.85 * bandTotal)
            run += sorted[(size_t) needed++];
        out.density[(size_t) f] = (float) std::log2 (std::max (1, needed) * binHz);
    }

    // Fine level envelope (20 ms windows every 10 ms) for sustain measurements.
    const int envHop = std::max (1, (int) (0.010 * sampleRate));
    const int envWin = 2 * envHop;
    const size_t envCount = numSamples >= (size_t) envWin ? (numSamples - (size_t) envWin) / (size_t) envHop + 1 : 0;
    out.envDb.resize (envCount);

    for (size_t i = 0; i < envCount; ++i)
    {
        const float* s = x + i * (size_t) envHop;
        double sq = 0.0;
        for (int j = 0; j < envWin; ++j)
            sq += (double) s[j] * s[j];
        out.envDb[i] = powerToDb ((float) (sq / envWin));
    }

    return out;
}

/** Median decay rate (dB/s) of held notes: from 50 ms after each attack until
    the next attack or until the note has faded. */
static bool measureSustain (const std::vector<float>& env, float& slopeOut)
{
    const int n = (int) env.size();
    if (n < 60)
        return false;

    const float loud = percentile (env, 0.95f);
    const float quiet = loud - 25.0f;

    std::vector<int> onsets;
    for (int i = 3; i < n; ++i)
        if (env[(size_t) i] > quiet && env[(size_t) i] - env[(size_t) i - 3] > 3.0f
            && (onsets.empty() || i - onsets.back() > 5))
            onsets.push_back (i);

    if (onsets.empty())
        onsets.push_back (0);

    std::vector<std::pair<float, float>> slopes; // slope, weight
    float totalWeight = 0.0f;

    for (size_t k = 0; k < onsets.size(); ++k)
    {
        const int start = onsets[k] + 5;
        int end = (k + 1 < onsets.size() ? onsets[k + 1] - 2 : n);

        for (int i = start; i < end; ++i)
            if (env[(size_t) i] < quiet)
            {
                end = i;
                break;
            }

        const int len = end - start;
        if (len < 15)
            continue;

        double st = 0, sy = 0, stt = 0, sty = 0;
        for (int i = start; i < end; ++i)
        {
            const double t = (i - start) * 0.010;
            st += t;
            sy += env[(size_t) i];
            stt += t * t;
            sty += t * env[(size_t) i];
        }
        const double denom = len * stt - st * st;
        if (denom <= 1.0e-9)
            continue;

        const float slope = std::clamp ((float) ((len * sty - st * sy) / denom), -80.0f, 5.0f);
        slopes.push_back ({ slope, (float) len });
        totalWeight += (float) len;
    }

    if (slopes.size() < 2 || totalWeight < 60.0f)
        return false;

    std::sort (slopes.begin(), slopes.end());
    float acc = 0.0f;
    for (const auto& s : slopes)
    {
        acc += s.second;
        if (acc >= 0.5f * totalWeight)
        {
            slopeOut = s.first;
            return true;
        }
    }
    return false;
}

ToneFeatures summarizeTone (const FrameAnalysis& fr)
{
    ToneFeatures t;
    t.ltasDb.fill (-200.0f);

    if (fr.numFrames <= 0)
        return t;

    const float p95 = percentile (fr.rmsDb, 0.95f);
    t.loudDb = p95;

    const float activeThresh = std::max (p95 - 30.0f, -75.0f);
    const float loudThresh = p95 - 12.0f;

    std::array<double, kNumBands> acc {};
    std::vector<float> levels, crests, densities;
    int numActive = 0;

    for (int f = 0; f < fr.numFrames; ++f)
    {
        const float lv = fr.rmsDb[(size_t) f];
        if (lv < activeThresh)
            continue;

        ++numActive;
        levels.push_back (lv);

        const float* psd = &fr.bandPsd[(size_t) f * kNumBands];
        for (int b = 0; b < kNumBands; ++b)
            acc[(size_t) b] += psd[b];

        if (lv >= loudThresh)
        {
            crests.push_back (fr.crestDb[(size_t) f]);
            densities.push_back (fr.density[(size_t) f]);
        }
    }

    if (numActive == 0)
        return t;

    for (int b = 0; b < kNumBands; ++b)
        t.ltasDb[(size_t) b] = powerToDb ((float) (acc[(size_t) b] / numActive));

    t.activeSeconds = (float) (numActive * (double) fr.hop / fr.sampleRate);
    t.levelSpreadDb = percentile (levels, 0.9f) - percentile (levels, 0.1f);
    t.crestDb = percentile (crests, 0.25f);
    t.density = percentile (densities, 0.5f);
    t.sustainValid = measureSustain (fr.envDb, t.sustainDbPerSec);

    return t;
}

float crestAtDensity (const FrameAnalysis& fr, float density)
{
    if (fr.numFrames <= 0)
        return 0.0f;

    const float loudThresh = percentile (fr.rmsDb, 0.95f) - 12.0f;

    std::vector<std::pair<float, float>> byDistance; // |density difference|, crest
    for (int f = 0; f < fr.numFrames; ++f)
        if (fr.rmsDb[(size_t) f] >= loudThresh)
            byDistance.push_back ({ std::abs (fr.density[(size_t) f] - density), fr.crestDb[(size_t) f] });

    if (byDistance.empty())
        return 0.0f;

    // the third of the frames that resembles the requested density most
    const size_t keep = std::max<size_t> (1, byDistance.size() / 3);
    std::partial_sort (byDistance.begin(), byDistance.begin() + (std::ptrdiff_t) keep, byDistance.end());

    std::vector<float> crests;
    for (size_t i = 0; i < keep; ++i)
        crests.push_back (byDistance[i].second);
    return percentile (crests, 0.25f);
}

ToneFeatures analyzeTone (const float* x, size_t numSamples, double sampleRate)
{
    return summarizeTone (analyzeFrames (x, numSamples, sampleRate));
}

bool estimateReverb (const float* x, size_t numSamples, double sampleRate, float& mix, float& decaySeconds)
{
    const int hop = std::max (1, (int) (0.010 * sampleRate));
    const int win = 2 * hop;
    if (numSamples < (size_t) (win + 100 * hop))
        return false;

    const int n = (int) ((numSamples - (size_t) win) / (size_t) hop);
    std::vector<float> env ((size_t) n);

    for (int i = 0; i < n; ++i)
    {
        double sq = 0.0;
        const float* s = x + (size_t) i * (size_t) hop;
        for (int j = 0; j < win; ++j)
            sq += (double) s[j] * s[j];
        env[(size_t) i] = powerToDb ((float) (sq / win));
    }

    const float loud = percentile (env, 0.95f);
    const float floorDb = std::max (percentile (env, 0.02f), loud - 70.0f);

    std::vector<float> tailLevels, slopes;

    // Look for abrupt stops: a stable loud level that suddenly falls away.
    for (int i = 12; i + 70 < n; ++i)
    {
        float pre = 0.0f;
        for (int j = i - 10; j < i; ++j)
            pre += env[(size_t) j];
        pre /= 10.0f;

        if (pre < loud - 10.0f || pre - env[(size_t) i + 6] < 3.5f)
            continue;

        // the level must have been steady before the stop (rules out pick attacks)
        float preMin = 1.0e9f, preMax = -1.0e9f;
        for (int k = i - 10; k < i; ++k)
        {
            preMin = std::min (preMin, env[(size_t) k]);
            preMax = std::max (preMax, env[(size_t) k]);
        }
        if (preMax - preMin > 4.0f)
            continue;

        // skip the fast part of the fall to find where the tail begins
        int j = i + 3;
        while (j < i + 25 && env[(size_t) j] - env[(size_t) j + 2] > 1.5f)
            ++j;

        float maxRate = 0.0f; // steepest 30 ms of the fall, dB/s
        for (int k = i - 2; k <= j; ++k)
            maxRate = std::max (maxRate, (env[(size_t) k] - env[(size_t) k + 3]) / 0.03f);

        // nothing new may start during the tail
        bool clean = true;
        for (int k = j; k < j + 30; ++k)
            if (env[(size_t) k] > env[(size_t) j] + 3.0f)
            {
                clean = false;
                break;
            }
        if (! clean)
            continue;

        const float tail = (env[(size_t) j] + env[(size_t) j + 1] + env[(size_t) j + 2]) / 3.0f;

        // linear regression of the tail's level over time
        double st = 0, sy = 0, stt = 0, sty = 0;
        int cnt = 0;
        for (int k = j; k < j + 24; ++k)
        {
            if (env[(size_t) k] < floorDb + 3.0f)
                break;
            const double tt = (k - j) * 0.010;
            st += tt;
            sy += env[(size_t) k];
            stt += tt * tt;
            sty += tt * env[(size_t) k];
            ++cnt;
        }

        const double denom = cnt * stt - st * st;
        const double slope = cnt >= 6 && denom > 1.0e-9 ? (cnt * sty - st * sy) / denom : -600.0;

        // Reverb shows as a knee: an abrupt drop, then a much slower decay.
        // A note that simply dies away at one rate is not reverb.
        if (maxRate < 120.0f)
            continue; // a gradual fade tells us nothing

        if (tail > floorDb + 6.0f && cnt >= 6 && maxRate >= 2.5 * -slope)
        {
            tailLevels.push_back (tail - pre);
            slopes.push_back ((float) slope);
        }
        else if (tail <= floorDb + 6.0f || -slope > 200.0)
        {
            // fell straight to the noise floor: evidence of a dry signal
            tailLevels.push_back (-60.0f);
            slopes.push_back (-600.0f);
        }

        i = j + 30;
    }

    if (tailLevels.size() < 3)
        return false;

    const float tailDb = percentile (tailLevels, 0.35f); // err on the dry side
    const float slope = percentile (slopes, 0.5f); // dB per second (negative)

    const float rt60 = slope < -1.0f ? -60.0f / slope : 6.0f;

    if (rt60 < 0.3f || tailDb < -40.0f)
    {
        mix = 0.0f;
        decaySeconds = 1.0f;
        return true;
    }

    // Invert the engine reverb's mix law: tail/dry = mix / (1 - mix/2).
    const float ratio = dbToGain (tailDb);
    decaySeconds = std::clamp (rt60, 0.4f, 5.0f);
    mix = std::clamp (ratio / (1.0f + 0.5f * ratio), 0.0f, 0.4f);
    return true;
}

} // namespace tg
