#pragma once

// Synthetic material for tests and experiments: a clean guitar "amped" through
// a known chain, plus bass, drums and a vocal-like source to build full mixes.

#include "tonegenie/DspBlocks.h"
#include "tonegenie/FrontEnd.h"
#include "tonegenie/SyntheticGuitar.h"
#include "tonegenie/ToneEngine.h"

#include <cmath>
#include <vector>

namespace tgtest
{

using Signal = std::vector<float>;

inline float rmsDb (const Signal& s)
{
    double sq = 0.0;
    for (float v : s)
        sq += (double) v * v;
    return 10.0f * (float) std::log10 (std::max (sq / (double) std::max<size_t> (1, s.size()), 1.0e-20));
}

inline void scale (Signal& s, float gain)
{
    for (auto& v : s)
        v *= gain;
}

inline void normalise (Signal& s, float targetRmsDb)
{
    scale (s, tg::dbToGain (targetRmsDb - rmsDb (s)));
}

inline Signal cleanGuitar (int phrase, double sr, const tg::GuitarVoice& voice = {}, int repeats = 1)
{
    double len = 0.0;
    const auto ev = phrase < 0 ? tg::calibrationPhrase (len) : tg::testPhrase (phrase, len);
    const auto once = tg::renderGuitar (ev, sr, len, voice);
    Signal out;
    for (int i = 0; i < repeats; ++i)
        out.insert (out.end(), once.begin(), once.end());
    return out;
}

/** A guitar amp + cabinet that is NOT the plugin's own model: different
    pre-shaping, a different clipper and a fixed speaker-like filter. */
struct TestAmp
{
    float gainDb = 30.0f;
    bool useEngineDrive = false; ///< true: use the plugin's own drive stage (self-consistency tests)
    int voicing = tg::voicingCrunch;
    float drive = 0.5f;
    float cabLowPassHz = 4500.0f;
    float presenceDb = 5.0f;

    Signal process (Signal x, double sr) const
    {
        normalise (x, -20.0f);

        if (useEngineDrive)
        {
            tg::DriveStage stage;
            stage.prepare (sr);
            stage.set (voicing, drive);
            stage.prepare (sr);
            for (size_t pos = 0; pos < x.size(); pos += tg::kMaxChunk)
                stage.process (x.data() + pos, (int) std::min ((size_t) tg::kMaxChunk, x.size() - pos));
        }
        else
        {
            // 8x oversampled arctan clipper with a mid-boost in front
            const int os = 8;
            tg::Biquad pre, aa1, aa2, aa3, aa4;
            pre.setPeak (900.0, 0.7, 6.0, sr);
            aa1.setLowPass (0.4 * sr, 0.54, sr * os);
            aa2.setLowPass (0.4 * sr, 1.31, sr * os);
            aa3.setLowPass (0.4 * sr, 0.54, sr * os);
            aa4.setLowPass (0.4 * sr, 1.31, sr * os);
            tg::OnePole hp;
            hp.setCutoff (120.0, sr);
            const float g = tg::dbToGain (gainDb);

            for (auto& v : x)
            {
                const float in = pre.process (hp.hp (v));
                float acc = 0.0f;
                for (int k = 0; k < os; ++k)
                {
                    float u = aa2.process (aa1.process (k == 0 ? in * (float) os : 0.0f));
                    u = std::atan (u * g) * 0.6366f;
                    u = aa4.process (aa3.process (u));
                    if (k == 0)
                        acc = u;
                }
                v = acc * 0.3f;
            }
        }

        // speaker cabinet
        tg::Biquad hp, lp1, lp2, pres, dip;
        hp.setHighPass (95.0, 0.9, sr);
        lp1.setLowPass (cabLowPassHz, 0.8, sr);
        lp2.setLowPass (cabLowPassHz * 1.15, 0.6, sr);
        pres.setPeak (2800.0, 1.2, presenceDb, sr);
        dip.setPeak (480.0, 1.0, -3.0, sr);
        for (auto& v : x)
            v = lp2.process (lp1.process (pres.process (dip.process (hp.process (v)))));

        return x;
    }
};

inline Signal bassLine (double sr, double seconds)
{
    std::vector<tg::NoteEvent> ev;
    const int notes[] = { 28, 28, 31, 33, 28, 28, 35, 33 };
    int i = 0;
    for (double t = 0.02; t < seconds - 0.5; t += 0.5)
        ev.push_back (tg::makeNote (t, 0.45, 0.9f, { notes[i++ % 8] }));

    tg::GuitarVoice v;
    v.pickupHz = 700.0f;
    v.pickupQ = 0.8f;
    v.pickBrightness = 0.2f;
    v.seed = 77;
    auto s = tg::renderGuitar (ev, sr, seconds, v);
    // renderGuitar high-passes at 70 Hz; put the fundamental back
    tg::Biquad shelf;
    shelf.setPeak (55.0, 0.7, 12.0, sr);
    for (auto& x : s)
        x = shelf.process (x);
    return s;
}

inline Signal drums (double sr, double seconds)
{
    Signal out ((size_t) (seconds * sr), 0.0f);
    tg::Rng rng (5);
    const double beat = 0.5;

    const auto add = [&] (double t, const Signal& hit)
    {
        const auto start = (size_t) (t * sr);
        for (size_t i = 0; i < hit.size() && start + i < out.size(); ++i)
            out[start + i] += hit[i];
    };

    Signal kick ((size_t) (0.3 * sr)), snare ((size_t) (0.2 * sr)), hat ((size_t) (0.06 * sr));

    double ph = 0.0;
    for (size_t i = 0; i < kick.size(); ++i)
    {
        const double t = i / sr;
        const double f = 45.0 + 90.0 * std::exp (-t / 0.03);
        ph += 2.0 * 3.14159265358979 * f / sr;
        kick[i] = (float) (0.9 * std::sin (ph) * std::exp (-t / 0.09));
    }

    tg::OnePole snHp;
    snHp.setCutoff (1500.0, sr);
    for (size_t i = 0; i < snare.size(); ++i)
    {
        const double t = i / sr;
        snare[i] = (float) (0.5 * snHp.hp (rng.next()) * std::exp (-t / 0.05)
                            + 0.4 * std::sin (2.0 * 3.14159265358979 * 190.0 * t) * std::exp (-t / 0.04));
    }

    tg::Biquad hatHp;
    hatHp.setHighPass (8000.0, 0.7, sr);
    for (size_t i = 0; i < hat.size(); ++i)
        hat[i] = (float) (0.35 * hatHp.process (rng.next()) * std::exp (-(i / sr) / 0.012));

    int n = 0;
    for (double t = 0.0; t < seconds; t += beat / 2, ++n)
    {
        add (t, hat);
        if (n % 4 == 0)
            add (t, kick);
        if (n % 4 == 2)
            add (t, snare);
    }
    return out;
}

inline Signal vocal (double sr, double seconds)
{
    Signal out ((size_t) (seconds * sr));
    tg::Biquad f1, f2, f3;
    f1.setBandPass (700.0, 4.0, sr);
    f2.setBandPass (1250.0, 5.0, sr);
    f3.setBandPass (2700.0, 6.0, sr);
    tg::Biquad sHp;
    sHp.setHighPass (6000.0, 0.7, sr);
    tg::Rng rng (9);

    double phase = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = i / sr;
        const double note = 220.0 * std::pow (2.0, (double) ((int) (t / 0.75) % 5) * 2.0 / 12.0);
        const double f = note * (1.0 + 0.012 * std::sin (2.0 * 3.14159265358979 * 5.5 * t));
        phase += f / sr;
        phase -= std::floor (phase);
        const float saw = (float) (2.0 * phase - 1.0);
        const double syl = std::fmod (t, 0.75);
        const float env = (float) (std::min (1.0, syl / 0.04) * (syl < 0.6 ? 1.0 : std::max (0.0, 1.0 - (syl - 0.6) / 0.1)));
        const float sib = syl < 0.05 ? 0.25f * sHp.process (rng.next()) : 0.0f;
        out[i] = env * (f1.process (saw) + 0.7f * f2.process (saw) + 0.4f * f3.process (saw)) * 0.6f + sib;
    }
    return out;
}

struct StereoMix
{
    Signal left, right;
};

/** Guitar panned to `guitarPan` (-1..1), everything else in the centre. */
inline StereoMix makeMix (const Signal& guitar, double sr, float guitarPan, float guitarLevelDb = -3.0f)
{
    const double secs = guitar.size() / sr;
    auto b = bassLine (sr, secs);
    auto d = drums (sr, secs);
    auto v = vocal (sr, secs);
    auto g = guitar;

    normalise (b, -20.0f);
    normalise (d, -18.0f);
    normalise (v, -21.0f);
    normalise (g, -20.0f + guitarLevelDb);

    const float gl = std::sqrt (0.5f * (1.0f - guitarPan)), gr = std::sqrt (0.5f * (1.0f + guitarPan));

    StereoMix m;
    m.left.resize (g.size());
    m.right.resize (g.size());
    for (size_t i = 0; i < g.size(); ++i)
    {
        const float c = 0.7071f * (b[std::min (i, b.size() - 1)] + d[std::min (i, d.size() - 1)] + v[std::min (i, v.size() - 1)]);
        m.left[i] = c + gl * g[i] * 1.4142f;
        m.right[i] = c + gr * g[i] * 1.4142f;
    }
    return m;
}

/** Runs a mono signal through the complete engine (synchronous filter design). */
inline Signal runEngine (tg::ToneEngine& engine, const Signal& in)
{
    Signal outL (in.size()), outR (in.size());
    const int block = 256;
    for (size_t pos = 0; pos < in.size(); pos += block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - pos);
        engine.process (in.data() + pos, nullptr, outL.data() + pos, outR.data() + pos, n);
    }
    for (size_t i = 0; i < in.size(); ++i)
        outL[i] = 0.5f * (outL[i] + outR[i]);
    return outL;
}

} // namespace tgtest
