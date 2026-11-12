#include "tonegenie/Reference.h"
#include "tonegenie/Fft.h"
#include "tonegenie/ToneFitter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace tg
{

//==============================================================================
bool MixAnalyzer::analyze (const float* left, const float* right, size_t numSamples, double sampleRate, const ProgressFn& progress)
{
    sr = sampleRate;
    const int n = analysisFftSize (sr);
    hop = n;
    numFrames = (int) (numSamples / (size_t) n);

    total.assign ((size_t) numFrames, 0.0f);
    sub.assign ((size_t) numFrames, 0.0f);
    air.assign ((size_t) numFrames, 0.0f);
    mid.assign ((size_t) numFrames, 0.0f);
    side.assign ((size_t) numFrames, 0.0f);
    thirds.assign ((size_t) numFrames * kThirdBands, 0.0f);

    if (numFrames == 0)
        return true;

    Fft fft (n);
    std::vector<std::complex<float>> buf ((size_t) n);
    std::vector<float> window ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * 3.14159265f * (float) i / (float) n);

    const double binHz = sr / n;
    const auto binFor = [&] (double hz) { return std::clamp ((int) std::lround (hz / binHz), 1, n / 2); };

    const int kTotLo = binFor (22.0), kTotHi = binFor (16000.0);
    const int kSubHi = binFor (60.0);
    const int kAirLo = binFor (9000.0);

    int thirdEdge[kThirdBands + 1];
    for (int i = 0; i <= kThirdBands; ++i)
        thirdEdge[i] = binFor (40.0 * std::pow (2.0, (i - 0.5) / 3.0));

    for (int f = 0; f < numFrames; ++f)
    {
        const size_t off = (size_t) f * (size_t) n;
        double m2 = 0.0, s2 = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const float l = left[off + (size_t) i];
            const float r = right != nullptr ? right[off + (size_t) i] : l;
            const float m = 0.5f * (l + r), s = 0.5f * (l - r);
            m2 += (double) m * m;
            s2 += (double) s * s;
            buf[(size_t) i] = { m * window[(size_t) i], 0.0f };
        }

        mid[(size_t) f] = (float) (m2 / n);
        side[(size_t) f] = (float) (s2 / n);

        fft.forward (buf.data());

        double t = 0.0, sb = 0.0, ar = 0.0;
        for (int k = kTotLo; k <= kTotHi; ++k)
        {
            const double p = std::norm (buf[(size_t) k]);
            t += p;
            if (k <= kSubHi)
                sb += p;
            if (k >= kAirLo)
                ar += p;
        }

        total[(size_t) f] = (float) t;
        sub[(size_t) f] = (float) sb;
        air[(size_t) f] = (float) ar;

        float* th = &thirds[(size_t) f * kThirdBands];
        for (int b = 0; b < kThirdBands; ++b)
        {
            double p = 0.0;
            for (int k = thirdEdge[b]; k < std::max (thirdEdge[b] + 1, thirdEdge[b + 1]); ++k)
                p += std::norm (buf[(size_t) std::min (k, n / 2)]);
            th[b] = (float) p;
        }

        if (progress && (f & 63) == 0 && ! progress ((float) f / (float) numFrames))
            return false;
    }

    return true;
}

MixVerdict MixAnalyzer::verdictForRange (int first, int end) const
{
    MixVerdict v;
    first = std::clamp (first, 0, numFrames);
    end = std::clamp (end, first, numFrames);
    if (end <= first)
        return v;

    float peak = 0.0f;
    for (int f = first; f < end; ++f)
        peak = std::max (peak, total[(size_t) f]);

    const float gate = peak * 1.0e-4f; // ignore silence (40 dB below the loudest frame)
    double t = 0, sb = 0, ar = 0, md = 0, sd = 0;
    double th[kThirdBands] = {};
    int count = 0;

    for (int f = first; f < end; ++f)
    {
        if (total[(size_t) f] < gate || total[(size_t) f] <= 0.0f)
            continue;

        t += total[(size_t) f];
        sb += sub[(size_t) f];
        ar += air[(size_t) f];
        md += mid[(size_t) f];
        sd += side[(size_t) f];
        for (int b = 0; b < kThirdBands; ++b)
            th[b] += thirds[(size_t) f * kThirdBands + (size_t) b];
        ++count;
    }

    if (count == 0 || t <= 0.0)
        return v;

    auto& ft = v.features;
    ft.subDb = (float) (10.0 * std::log10 (std::max (sb / t, 1.0e-10)));
    ft.airDb = sr > 24000.0 ? (float) (10.0 * std::log10 (std::max (ar / t, 1.0e-10))) : -100.0f;
    ft.sideDb = (float) (10.0 * std::log10 (std::max (sd / std::max (md, 1.0e-20), 1.0e-10)));
    ft.levelDb = (float) (10.0 * std::log10 (std::max (md / count, 1.0e-20)));

    double thPeak = 0.0;
    for (double p : th)
        thPeak = std::max (thPeak, p);
    int occupied = 0;
    for (double p : th)
        if (p > thPeak * 3.16e-4) // within 35 dB of the strongest band
            ++occupied;
    ft.occupancy = (float) occupied / (float) kThirdBands;

    // Hand-tuned logistic model. A lone guitar has (almost) nothing below its
    // lowest string and little above its speaker's roll-off; a full mix fills
    // the whole spectrum with kick, bass and cymbals.
    const auto lim = [] (float x) { return std::clamp (x, -12.0f, 12.0f); };

    float z = 0.32f * lim (ft.subDb + 27.0f)
            + 8.0f * (ft.occupancy - 0.86f);

    if (sr > 24000.0)
        z += 0.16f * lim (ft.airDb + 33.0f);

    v.probability = 1.0f / (1.0f + std::exp (-z));
    v.fullMix = v.probability >= 0.5f;
    return v;
}

std::vector<MixSegment> MixAnalyzer::segments (double segmentSeconds) const
{
    std::vector<MixSegment> out;
    const int per = std::max (1, (int) std::lround (segmentSeconds * sr / hop));

    for (int f = 0; f + per <= numFrames; f += per)
    {
        MixSegment s;
        s.startSec = (double) f * hop / sr;
        s.endSec = (double) (f + per) * hop / sr;
        s.verdict = verdictForRange (f, f + per);
        out.push_back (s);
    }
    return out;
}

//==============================================================================
static float medianOf (float* v, int n) noexcept
{
    std::nth_element (v, v + n / 2, v + n);
    return v[n / 2];
}

IsolationResult isolateGuitar (const float* left, const float* right, size_t numSamples, double sampleRate,
                               Focus focus, const ProgressFn& progress)
{
    IsolationResult res;

    const int n = analysisFftSize (sampleRate);
    const int hop = n / 4;
    if (numSamples < (size_t) (4 * n))
        return res;

    const bool mono = right == nullptr;
    const double binHz = sampleRate / n;
    const int numBins = std::min (n / 2, (int) std::ceil (9500.0 / binHz)) + 1;

    const size_t padded = numSamples + 2 * (size_t) n;
    const int numFrames = (int) ((padded - (size_t) n) / (size_t) hop) + 1;

    const auto report = [&] (float p) { return ! progress || progress (p); };

    std::vector<float> window ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (2.0f * 3.14159265f * (float) i / (float) n));

    Fft fft (n);
    std::vector<std::complex<float>> buf ((size_t) n);

    const size_t cells = (size_t) numFrames * (size_t) numBins;
    std::vector<std::complex<float>> specL (cells), specR (mono ? 0 : cells);
    std::vector<float> mag (cells);

    // --- analysis ---------------------------------------------------------------
    const auto sampleAt = [&] (const float* ch, long long idx) -> float
    {
        idx -= n;
        return (idx >= 0 && idx < (long long) numSamples) ? ch[idx] : 0.0f;
    };

    for (int f = 0; f < numFrames; ++f)
    {
        const long long start = (long long) f * hop;

        for (int c = 0; c < (mono ? 1 : 2); ++c)
        {
            const float* ch = c == 0 ? left : right;
            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = { sampleAt (ch, start + i) * window[(size_t) i], 0.0f };

            fft.forward (buf.data());

            auto* dst = (c == 0 ? specL.data() : specR.data()) + (size_t) f * (size_t) numBins;
            std::copy (buf.begin(), buf.begin() + numBins, dst);
        }

        float* m = &mag[(size_t) f * (size_t) numBins];
        for (int k = 0; k < numBins; ++k)
        {
            const float pl = std::norm (specL[(size_t) f * (size_t) numBins + (size_t) k]);
            const float pr = mono ? pl : std::norm (specR[(size_t) f * (size_t) numBins + (size_t) k]);
            m[k] = std::sqrt (pl + pr);
        }

        if ((f & 127) == 0 && ! report (0.3f * (float) f / (float) numFrames))
            return res;
    }

    // --- harmonic / percussive masks ----------------------------------------------
    // harmonic = median along time, percussive = median along frequency
    constexpr int tHalf = 8, fHalf = 15;
    std::vector<float> harm (cells), mask (cells);

    {
        std::vector<float> col ((size_t) numFrames);
        float tmp[2 * tHalf + 1];

        for (int k = 0; k < numBins; ++k)
        {
            for (int f = 0; f < numFrames; ++f)
                col[(size_t) f] = mag[(size_t) f * (size_t) numBins + (size_t) k];

            for (int f = 0; f < numFrames; ++f)
            {
                const int a = std::max (0, f - tHalf), b = std::min (numFrames - 1, f + tHalf);
                std::copy (col.begin() + a, col.begin() + b + 1, tmp);
                harm[(size_t) f * (size_t) numBins + (size_t) k] = medianOf (tmp, b - a + 1);
            }

            if ((k & 127) == 0 && ! report (0.3f + 0.2f * (float) k / (float) numBins))
                return res;
        }
    }

    {
        float tmp[2 * fHalf + 1];

        for (int f = 0; f < numFrames; ++f)
        {
            const float* m = &mag[(size_t) f * (size_t) numBins];
            for (int k = 0; k < numBins; ++k)
            {
                const int a = std::max (0, k - fHalf), b = std::min (numBins - 1, k + fHalf);
                std::copy (m + a, m + b + 1, tmp);
                const float perc = medianOf (tmp, b - a + 1);
                const float h = harm[(size_t) f * (size_t) numBins + (size_t) k];
                mask[(size_t) f * (size_t) numBins + (size_t) k] = h * h / (h * h + perc * perc + 1.0e-20f);
            }

            if ((f & 127) == 0 && ! report (0.5f + 0.25f * (float) f / (float) numFrames))
                return res;
        }
    }

    // --- where does the guitar sit in the stereo field? ---------------------------
    int side = 0; // -1 left, 0 centre, +1 right

    if (! mono)
    {
        constexpr int hbins = 41;
        double hist[hbins] = {};
        const int kLo = std::max (1, (int) (250.0 / binHz)), kHi = std::min (numBins - 1, (int) (4000.0 / binHz));

        for (int f = 0; f < numFrames; ++f)
            for (int k = kLo; k <= kHi; ++k)
            {
                const size_t idx = (size_t) f * (size_t) numBins + (size_t) k;
                const float al = std::abs (specL[idx]), ar = std::abs (specR[idx]);
                const float p = (ar - al) / (ar + al + 1.0e-20f);
                const int hb = std::clamp ((int) std::lround ((p + 1.0f) * 0.5f * (hbins - 1)), 0, hbins - 1);
                hist[hb] += (double) mag[idx] * mag[idx] * mask[idx];
            }

        double sum = 0.0, sm[hbins];
        for (int i = 0; i < hbins; ++i)
        {
            sm[i] = 0.25 * hist[std::max (0, i - 1)] + 0.5 * hist[i] + 0.25 * hist[std::min (hbins - 1, i + 1)];
            sum += hist[i];
        }

        const auto panOf = [] (int i) { return 2.0f * (float) i / (float) (hbins - 1) - 1.0f; };

        int best = -1;
        for (int i = 0; i < hbins; ++i)
            if (std::abs (panOf (i)) >= 0.25f && (best < 0 || sm[i] > sm[best]))
                best = i;

        double cluster = 0.0;
        for (int i = 0; i < hbins; ++i)
            if (std::abs (panOf (i) - panOf (best)) <= 0.2f)
                cluster += hist[i];

        const float clusterShare = sum > 0.0 ? (float) (cluster / sum) : 0.0f;

        switch (focus)
        {
            case Focus::left:   side = -1; break;
            case Focus::right:  side = 1; break;
            case Focus::center: side = 0; break;
            case Focus::automatic:
            default:
                side = clusterShare >= 0.10f ? (panOf (best) < 0.0f ? -1 : 1) : 0;
                res.pan = side != 0 ? panOf (best) : 0.0f;
                break;
        }

        if (focus != Focus::automatic)
            res.pan = (float) side * 0.7f;
    }

    // Centre extraction cannot get rid of the bass guitar by position, so cut lower.
    const float loStart = side == 0 && ! mono ? 150.0f : 70.0f;
    const float loEnd = side == 0 && ! mono ? 250.0f : 105.0f;
    res.validLoHz = loEnd;
    res.validHiHz = std::min (7000.0f, (float) (0.42 * sampleRate));

    std::vector<float> bandWeight ((size_t) numBins);
    for (int k = 0; k < numBins; ++k)
    {
        const float hz = (float) (k * binHz);
        float w = std::clamp ((hz - loStart) / (loEnd - loStart), 0.0f, 1.0f);
        w *= std::clamp ((9000.0f - hz) / 2000.0f, 0.0f, 1.0f);
        bandWeight[(size_t) k] = w;
    }

    // --- resynthesis ------------------------------------------------------------------
    std::vector<float> out (padded, 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        std::fill (buf.begin(), buf.end(), std::complex<float>());

        for (int k = 1; k < numBins; ++k)
        {
            const size_t idx = (size_t) f * (size_t) numBins + (size_t) k;
            const auto l = specL[idx];
            std::complex<float> s;
            float panMask = 1.0f;

            if (mono)
            {
                s = l;
            }
            else
            {
                const auto r = specR[idx];
                const float al = std::abs (l), ar = std::abs (r);
                const float p = (ar - al) / (ar + al + 1.0e-20f);

                if (side == 0)
                {
                    s = 0.5f * (l + r);
                    panMask = std::exp (-(p * p) / 0.04f);
                }
                else
                {
                    // Subtracting the channels cancels everything panned to the middle
                    // (so bins dominated by bass or vocals still yield the guitar underneath).
                    // The mask only has to reject what sits on the opposite side.
                    s = side < 0 ? l - r : r - l;
                    const float t = std::clamp (((float) side * p + 0.4f) / 0.3f, 0.0f, 1.0f);
                    panMask = t * t * (3.0f - 2.0f * t);
                }
            }

            const float g = panMask * mask[idx] * bandWeight[(size_t) k];
            buf[(size_t) k] = s * g;
            buf[(size_t) (n - k)] = std::conj (buf[(size_t) k]);
        }

        fft.inverse (buf.data());

        float* dst = out.data() + (size_t) f * (size_t) hop;
        for (int i = 0; i < n; ++i)
            dst[i] += buf[(size_t) i].real() * window[(size_t) i] * 0.5f;

        if ((f & 127) == 0 && ! report (0.75f + 0.25f * (float) f / (float) numFrames))
            return res;
    }

    res.guitar.assign (out.begin() + n, out.begin() + n + (std::ptrdiff_t) numSamples);
    res.ok = true;
    return res;
}

//==============================================================================
static std::vector<float> monoSum (const float* left, const float* right, size_t start, size_t count)
{
    std::vector<float> m (count);
    for (size_t i = 0; i < count; ++i)
        m[i] = right != nullptr ? 0.5f * (left[start + i] + right[start + i]) : left[start + i];
    return m;
}

ReferenceResult analyzeReference (const float* left, const float* right, size_t numSamples, double sampleRate,
                                  const ReferenceOptions& options, const ProgressFn& progress)
{
    ReferenceResult res;
    res.guitarSampleRate = sampleRate;

    const auto sub = [&] (float from, float to)
    {
        return ProgressFn ([&progress, from, to] (float p) { return ! progress || progress (from + (to - from) * p); });
    };

    if (sampleRate < 8000.0 || numSamples < (size_t) (1.0 * sampleRate))
    {
        res.message = "The audio is too short to analyse (need at least a second).";
        return res;
    }

    // --- region ---------------------------------------------------------------------
    size_t s0 = 0, s1 = numSamples;
    const bool userRegion = options.regionStartSec >= 0.0 && options.regionEndSec > options.regionStartSec + 0.5;

    if (userRegion)
    {
        s0 = std::min ((size_t) (options.regionStartSec * sampleRate), numSamples);
        s1 = std::min ((size_t) (options.regionEndSec * sampleRate), numSamples);
        if (s1 < s0 + (size_t) sampleRate)
        {
            res.message = "The selected region is too short (need at least a second).";
            return res;
        }
    }

    // --- solo guitar or full mix? ------------------------------------------------------
    MixAnalyzer mixer;
    if (! mixer.analyze (left + s0, right != nullptr ? right + s0 : nullptr, s1 - s0, sampleRate, sub (0.0f, 0.08f)))
        return res;

    const auto verdict = mixer.verdict();
    const bool fullMix = options.mode == SourceMode::automatic ? verdict.fullMix : options.mode == SourceMode::fullMix;

    auto& prof = res.profile;
    prof.fromFullMix = fullMix;
    prof.mixProbability = verdict.probability;

    const size_t maxPlain = (size_t) (120.0 * sampleRate);
    const size_t maxIsolate = (size_t) (45.0 * sampleRate);

    if (! fullMix)
    {
        const size_t count = std::min (s1 - s0, maxPlain);
        res.guitar = monoSum (left, right, s0, count);
        prof.usedStartSec = s0 / sampleRate;
        prof.usedEndSec = (s0 + count) / sampleRate;
        prof.validLoHz = 60.0f;
        prof.validHiHz = (float) std::min (16000.0, 0.45 * sampleRate);
    }
    else
    {
        const auto segs = mixer.segments (3.0);

        // 1. Is the guitar exposed anywhere (intro, break)? Then no separation is needed.
        if (! userRegion && options.mode == SourceMode::automatic)
        {
            float maxLevel = -200.0f;
            for (const auto& s : segs)
                maxLevel = std::max (maxLevel, s.verdict.features.levelDb);

            std::vector<const MixSegment*> exposed;
            double exposedSecs = 0.0;
            for (const auto& s : segs)
                if (s.verdict.probability < 0.2f && s.verdict.features.levelDb > maxLevel - 25.0f)
                {
                    exposed.push_back (&s);
                    exposedSecs += s.endSec - s.startSec;
                }

            if (exposedSecs >= 6.0)
            {
                for (const auto* s : exposed)
                {
                    const size_t a = s0 + (size_t) (s->startSec * sampleRate);
                    const size_t b = std::min (s1, s0 + (size_t) (s->endSec * sampleRate));
                    if (res.guitar.size() + (b - a) > maxPlain)
                        break;
                    const auto part = monoSum (left, right, a, b - a);
                    res.guitar.insert (res.guitar.end(), part.begin(), part.end());
                }

                prof.usedExposedGuitar = true;
                prof.usedStartSec = s0 / sampleRate + exposed.front()->startSec;
                prof.usedEndSec = s0 / sampleRate + exposed.back()->endSec;
                prof.validLoHz = 60.0f;
                prof.validHiHz = (float) std::min (16000.0, 0.45 * sampleRate);
            }
        }

        // 2. Otherwise separate the guitar from the busiest stretch (or the user's region).
        if (! prof.usedExposedGuitar)
        {
            size_t a = s0, b = s1;

            if (b - a > maxIsolate)
            {
                const int per = std::max (1, (int) (45.0 / 3.0));
                int bestStart = 0;
                double bestSum = -1.0e30;

                for (int i = 0; i + per <= (int) segs.size(); ++i)
                {
                    double sum = 0.0;
                    for (int j = i; j < i + per; ++j)
                        sum += dbToPower (segs[(size_t) j].verdict.features.levelDb);
                    if (sum > bestSum)
                    {
                        bestSum = sum;
                        bestStart = i;
                    }
                }

                if (! segs.empty())
                    a = s0 + (size_t) (segs[(size_t) bestStart].startSec * sampleRate);
                b = std::min (s1, a + maxIsolate);
            }

            auto iso = isolateGuitar (left + a, right != nullptr ? right + a : nullptr, b - a, sampleRate,
                                      options.focus, sub (0.08f, 0.45f));
            if (! iso.ok)
            {
                if (res.message.empty())
                    res.message = "Could not isolate a guitar from this audio.";
                return res;
            }

            res.guitar = std::move (iso.guitar);
            prof.isolated = true;
            prof.isolationPan = iso.pan;
            prof.validLoHz = iso.validLoHz;
            prof.validHiHz = iso.validHiHz;
            prof.usedStartSec = a / sampleRate;
            prof.usedEndSec = b / sampleRate;
        }
    }

    // --- measure the tone -----------------------------------------------------------------
    prof.reference = analyzeTone (res.guitar.data(), res.guitar.size(), sampleRate);

    if (prof.reference.activeSeconds < 1.0f || prof.reference.loudDb < -70.0f)
    {
        res.message = "There is not enough guitar signal in this audio to learn a tone from.";
        return res;
    }

    prof.targetDb = prof.reference.ltasDb;

    float revMix = 0.0f, revDecay = 1.2f;
    if (estimateReverb (res.guitar.data(), res.guitar.size(), sampleRate, revMix, revDecay))
    {
        prof.reverbMix = std::min (revMix, prof.isolated ? 0.2f : 0.4f);
        prof.reverbDecay = revDecay;
    }
    else
    {
        prof.reverbMix = 0.05f; // unknown: just a touch of room
        prof.reverbDecay = 1.2f;
    }

    // --- find the pedal settings ------------------------------------------------------------
    ToneFitter fitter;
    if (! fitter.fit (prof, sub (0.45f, 1.0f)))
        return res;

    prof.valid = true;
    res.ok = true;

    static const char* const voiceNames[] = { "Smooth", "Crunch", "Fuzz" };
    char text[320];
    const char* source = ! fullMix ? "Solo guitar"
                       : prof.usedExposedGuitar ? "Full mix - used passages where the guitar plays alone"
                       : prof.isolationPan < -0.1f ? "Full mix - guitar isolated (left)"
                       : prof.isolationPan > 0.1f ? "Full mix - guitar isolated (right)"
                       : "Full mix - guitar isolated from the centre (rough: select a passage where the guitar is exposed for a better match)";

    std::snprintf (text, sizeof (text), "%s. Tone: %s, drive %d%%.", source,
                   voiceNames[std::clamp (prof.voicing, 0, 2)], (int) std::lround (prof.drive * 100.0f));
    res.message = text;
    return res;
}

} // namespace tg
