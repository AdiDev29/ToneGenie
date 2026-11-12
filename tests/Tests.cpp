// ToneGenie core test suite. Plain C++, no framework: every check that fails
// is printed and the process exits non-zero.
//
// Usage: tonegenie_tests [name-filter]

#include "TestSignals.h"
#include "WavIo.h"
#include "tonegenie/Analysis.h"
#include "tonegenie/Fft.h"
#include "tonegenie/MatchEq.h"
#include "tonegenie/Reference.h"
#include "tonegenie/ToneEngine.h"
#include "tonegenie/ToneFitter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>

using namespace tg;
using namespace tgtest;

namespace
{

int failures = 0, checks = 0;

#define CHECK(cond) \
    do { ++checks; if (! (cond)) { ++failures; std::printf ("    FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (false)

#define CHECK_MSG(cond, ...) \
    do { ++checks; if (! (cond)) { ++failures; std::printf ("    FAILED %s:%d  %s  [", __FILE__, __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("]\n"); } } while (false)

bool allFinite (const Signal& s)
{
    for (float v : s)
        if (! std::isfinite (v))
            return false;
    return true;
}

float peakOf (const Signal& s)
{
    float p = 0.0f;
    for (float v : s)
        p = std::max (p, std::abs (v));
    return p;
}

/** Mean / max absolute difference between two spectra after removing their average level. */
float spectrumError (const BandArray& a, const BandArray& b, float loHz, float hiHz, float* maxError = nullptr)
{
    const int b0 = (int) std::ceil (bandIndexForHz (loHz)), b1 = (int) std::floor (bandIndexForHz (hiHz));
    const int n = b1 - b0 + 1;
    float ma = 0.0f, mb = 0.0f;
    for (int i = b0; i <= b1; ++i)
    {
        ma += a[(size_t) i];
        mb += b[(size_t) i];
    }
    ma /= (float) n;
    mb /= (float) n;

    float sum = 0.0f, mx = 0.0f;
    for (int i = b0; i <= b1; ++i)
    {
        const float e = std::abs ((a[(size_t) i] - ma) - (b[(size_t) i] - mb));
        sum += e;
        mx = std::max (mx, e);
    }
    if (maxError != nullptr)
        *maxError = mx;
    return sum / (float) n;
}

Signal ampedReference (int phrase, float gainDb, double sr, int repeats = 2)
{
    TestAmp amp;
    amp.gainDb = gainDb;
    return amp.process (cleanGuitar (phrase, sr, {}, repeats), sr);
}

void applyProfile (ToneEngine& engine, const ToneProfile& p, bool adapt = true)
{
    EngineParams ep;
    ep.voicing = p.voicing;
    ep.drive = p.drive;
    ep.comp = p.comp;
    ep.gate = p.gate;
    ep.adapt = adapt;
    engine.setParams (ep);
    engine.setProfile (&p);
}

//==============================================================================
void testFft()
{
    Fft fft (1024);
    std::vector<std::complex<float>> a (1024), b;
    Rng rng (3);
    for (auto& v : a)
        v = { rng.next(), rng.next() };
    b = a;
    fft.forward (b.data());
    fft.inverse (b.data());

    float err = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        err = std::max (err, std::abs (a[i] - b[i]));
    CHECK_MSG (err < 1.0e-4f, "round-trip error %g", err);

    // a pure tone lands in exactly one bin
    for (int i = 0; i < 1024; ++i)
        a[(size_t) i] = { std::cos (2.0f * kPi * 37.0f * (float) i / 1024.0f), 0.0f };
    fft.forward (a.data());
    CHECK (std::abs (std::abs (a[37]) - 512.0f) < 0.5f);
    CHECK (std::abs (a[100]) < 0.05f);
}

void testBands()
{
    // white noise has a flat power density on the band grid
    const double sr = 48000.0;
    Rng rng (11);
    Signal noise ((size_t) (8 * sr));
    for (auto& v : noise)
        v = 0.25f * rng.next();

    const auto f = analyzeTone (noise.data(), noise.size(), sr);
    const int lo = (int) bandIndexForHz (60.0f), hi = (int) bandIndexForHz (16000.0f);
    float mn = 1.0e9f, mx = -1.0e9f;
    for (int b = lo; b <= hi; ++b)
    {
        mn = std::min (mn, f.ltasDb[(size_t) b]);
        mx = std::max (mx, f.ltasDb[(size_t) b]);
    }
    CHECK_MSG (mx - mn < 2.5f, "white noise band ripple %.2f dB", mx - mn);

    // PSD integrates back to the signal power: uniform noise of amplitude 0.25 has power 0.25^2/3
    const float expectedDb = powerToDb (0.25f * 0.25f / 3.0f / (float) (sr / 2.0));
    CHECK_MSG (std::abs (f.ltasDb[(size_t) (lo + hi) / 2] - expectedDb) < 1.0f, "psd %.1f expected %.1f", f.ltasDb[(size_t) (lo + hi) / 2], expectedDb);
}

void testMinPhaseDesign()
{
    const int taps = 1024;
    MinPhaseDesigner designer;
    designer.prepare (taps);
    const int n = designer.fftSize();

    // a cabinet-like response: +6 dB bump, steep roll-off
    std::vector<float> binDb ((size_t) n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k)
    {
        const float hz = (float) k * 48000.0f / (float) n;
        float db = 6.0f * std::exp (-0.5f * std::pow (std::log2 (std::max (hz, 1.0f) / 2500.0f) / 0.5f, 2.0f));
        if (hz > 5000.0f)
            db -= 24.0f * std::log2 (hz / 5000.0f);
        binDb[(size_t) k] = std::max (db, -50.0f);
    }

    std::vector<float> fir ((size_t) taps);
    designer.design (binDb.data(), fir.data());

    Fft fft (n);
    std::vector<std::complex<float>> buf ((size_t) n);
    for (int i = 0; i < taps; ++i)
        buf[(size_t) i] = { fir[(size_t) i], 0.0f };
    fft.forward (buf.data());

    float worst = 0.0f;
    for (int k = 4; k <= n / 2; ++k)
        if (binDb[(size_t) k] > -40.0f)
            worst = std::max (worst, std::abs (gainToDb (std::abs (buf[(size_t) k])) - binDb[(size_t) k]));
    CHECK_MSG (worst < 1.0f, "magnitude error %.2f dB", worst);

    // minimum phase: the energy sits at the start of the impulse response
    double early = 0.0, total = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        total += (double) fir[(size_t) i] * fir[(size_t) i];
        if (i < 64)
            early += (double) fir[(size_t) i] * fir[(size_t) i];
    }
    CHECK_MSG (early / total > 0.95, "early energy %.3f", early / total);
}

void testOversampler()
{
    Oversampler4x os;
    const int n = 4096;
    Signal in ((size_t) n), out ((size_t) n);
    for (int i = 0; i < n; ++i)
        in[(size_t) i] = std::sin (2.0f * kPi * 1000.0f * (float) i / 48000.0f);

    float up[4 * kMaxChunk];
    for (int pos = 0; pos < n; pos += kMaxChunk)
    {
        os.up (in.data() + pos, up, kMaxChunk);
        os.down (up, out.data() + pos, kMaxChunk);
    }

    // unity gain and exactly kLatency samples of delay
    float err = 0.0f;
    for (int i = 600; i < n; ++i)
        err = std::max (err, std::abs (out[(size_t) i] - in[(size_t) (i - Oversampler4x::kLatency)]));
    CHECK_MSG (err < 0.002f, "pass-band error %g", err);
}

void testEngineBasics()
{
    for (double sr : { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        ToneEngine engine;
        engine.setSynchronous (true);
        engine.prepare (sr);

        // fully dry: the input must come back unchanged, delayed by the reported latency
        EngineParams ep;
        ep.mix = 0.0f;
        engine.setParams (ep);

        Signal in ((size_t) (0.25 * sr), 0.0f), outL (in.size()), outR (in.size());
        in[100] = 1.0f;
        engine.process (in.data(), nullptr, outL.data(), outR.data(), (int) in.size());

        const size_t expected = 100 + (size_t) ToneEngine::latencySamples();
        CHECK_MSG (std::abs (outL[expected] - 1.0f) < 1.0e-5f && std::abs (outR[expected] - 1.0f) < 1.0e-5f, "dry latency at %.0f Hz", sr);
        CHECK (std::abs (outL[expected - 1]) < 1.0e-5f);

        // wet with every block size, mono and stereo, in-place
        ep.mix = 1.0f;
        ep.reverbMix = 0.3f;
        ep.delayMix = 0.3f;
        ep.comp = 0.5f;
        engine.setParams (ep);

        auto guitar = cleanGuitar (0, sr);
        guitar.resize ((size_t) (0.6 * sr));

        for (int block : { 1, 7, 64, 333, 4096 })
        {
            Signal l (guitar), r (guitar);
            for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
            {
                const int n = (int) std::min ((size_t) block, l.size() - pos);
                engine.process (l.data() + pos, r.data() + pos, l.data() + pos, r.data() + pos, n);
            }
            CHECK_MSG (allFinite (l) && allFinite (r), "finite output at %.0f Hz block %d", sr, block);
            CHECK_MSG (peakOf (l) < 4.0f, "sane level at %.0f Hz block %d: %.2f", sr, block, peakOf (l));

            Signal m (guitar);
            for (size_t pos = 0; pos < m.size(); pos += (size_t) block)
                engine.process (m.data() + pos, nullptr, m.data() + pos, nullptr, (int) std::min ((size_t) block, m.size() - pos));
            CHECK (allFinite (m));
        }
    }
}

void testEngineRobustness()
{
    const double sr = 48000.0;
    ToneEngine engine;
    engine.setSynchronous (true);
    engine.prepare (sr);

    EngineParams ep;
    ep.drive = 1.0f;
    ep.voicing = voicingFuzz;
    ep.gate = 0.5f;
    engine.setParams (ep);

    // silence in -> silence out, even at maximum gain
    Signal silence ((size_t) sr, 0.0f);
    auto out = runEngine (engine, silence);
    CHECK (allFinite (out));
    CHECK_MSG (peakOf (out) < 1.0e-4f, "silence -> %g", peakOf (out));

    // hostile input: full-scale square wave with DC, then NaN-free recovery
    Signal nasty ((size_t) sr);
    for (size_t i = 0; i < nasty.size(); ++i)
        nasty[i] = ((i / 100) % 2 ? 1.0f : -1.0f) + 0.5f;
    out = runEngine (engine, nasty);
    CHECK (allFinite (out));
    CHECK_MSG (peakOf (out) < 8.0f, "square wave peak %.2f", peakOf (out));

    // a NaN or Inf from upstream must not stick: the next clean input gives clean output
    {
        Signal bad ((size_t) (0.2 * sr), 0.1f);
        bad[100] = std::numeric_limits<float>::quiet_NaN();
        bad[200] = std::numeric_limits<float>::infinity();
        out = runEngine (engine, bad);
        CHECK (allFinite (out));

        auto clean = cleanGuitar (0, sr);
        clean.resize ((size_t) sr);
        out = runEngine (engine, clean);
        CHECK (allFinite (out));
        CHECK_MSG (rmsDb (out) > -50.0f, "engine silent after a NaN: %.1f dB", rmsDb (out));
    }

    // every voicing / drive / parameter corner
    auto guitar = cleanGuitar (2, sr);
    guitar.resize ((size_t) sr);
    for (int v = 0; v < numVoicings; ++v)
        for (float d : { 0.0f, 0.5f, 1.0f })
        {
            ep = EngineParams();
            ep.voicing = v;
            ep.drive = d;
            ep.comp = d;
            ep.match = 1.0f - d;
            ep.reverbMix = d;
            ep.reverbDecay = 0.2f + 7.0f * d;
            ep.delayMix = d;
            ep.delayFeedback = 0.9f;
            ep.delayTimeMs = 20.0f + 1100.0f * d;
            engine.setParams (ep);
            out = runEngine (engine, guitar);
            CHECK_MSG (allFinite (out) && peakOf (out) < 8.0f, "voicing %d drive %.1f peak %.2f", v, d, peakOf (out));
        }
}

void testBackgroundDesign()
{
    // Real-time mode: filter design happens on another thread while audio runs.
    const double sr = 48000.0;
    const auto ref = ampedReference (0, 30.0f, 44100.0);
    const auto res = analyzeReference (ref.data(), nullptr, ref.size(), 44100.0, {});
    CHECK (res.ok);

    ToneEngine engine;
    engine.prepare (sr);
    applyProfile (engine, res.profile);

    std::atomic<bool> stop { false };
    int designs = 0;
    std::thread worker ([&]
    {
        while (! stop.load())
        {
            if (engine.serviceBackground())
                ++designs;
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
    });

    GuitarVoice bright;
    bright.pickupHz = 6000.0f;
    bright.pickupQ = 2.5f;
    auto di = cleanGuitar (0, sr, bright, 3);
    normalise (di, -30.0f);

    Signal outL (di.size()), outR (di.size());
    for (size_t pos = 0; pos < di.size(); pos += 128)
    {
        engine.process (di.data() + pos, nullptr, outL.data() + pos, outR.data() + pos, (int) std::min ((size_t) 128, di.size() - pos));
        if ((pos / 128) % 32 == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1)); // let the design thread keep up
    }

    stop.store (true);
    worker.join();

    CHECK (allFinite (outL));
    CHECK_MSG (designs > 5, "only %d background designs", designs);

    Signal tail (outL.begin() + (std::ptrdiff_t) (outL.size() * 2 / 3), outL.end());
    const auto f = analyzeTone (tail.data(), tail.size(), sr);
    const float err = spectrumError (f.ltasDb, res.profile.targetDb, 100.0f, 5000.0f);
    CHECK_MSG (err < 2.0f, "background-designed EQ spectrum error %.2f dB", err);
}

void testClassifier()
{
    const double sr = 44100.0;
    GuitarVoice bright;
    bright.pickupHz = 6000.0f;
    bright.pickupQ = 2.5f;
    bright.pickBrightness = 0.9f;

    const auto prob = [&] (const Signal& l, const Signal* r)
    {
        MixAnalyzer m;
        m.analyze (l.data(), r != nullptr ? r->data() : nullptr, l.size(), sr);
        return m.verdict().probability;
    };

    for (int phrase = 0; phrase < 4; ++phrase)
    {
        auto di = cleanGuitar (phrase, sr, bright, 2);
        normalise (di, -20.0f);
        CHECK_MSG (prob (di, nullptr) < 0.3f, "clean DI phrase %d: %.2f", phrase, prob (di, nullptr));

        for (float gain : { 10.0f, 40.0f })
        {
            const auto amped = ampedReference (phrase, gain, sr);
            CHECK_MSG (prob (amped, nullptr) < 0.3f, "amped %.0f dB phrase %d: %.2f", gain, phrase, prob (amped, nullptr));
        }
    }

    const auto guitar = ampedReference (0, 36.0f, sr, 3);
    for (float pan : { -0.7f, 0.0f, 0.5f })
        for (float level : { -3.0f, 6.0f })
        {
            const auto mix = makeMix (guitar, sr, pan, level);
            const float p = prob (mix.left, &mix.right);
            CHECK_MSG (p > 0.7f, "mix pan %.1f level %.0f: %.2f", pan, level, p);
        }
}

void testIsolation()
{
    const double sr = 44100.0;
    const auto guitar = ampedReference (0, 36.0f, sr, 3);
    const auto truth = analyzeTone (guitar.data(), guitar.size(), sr);

    for (float pan : { -0.7f, -0.3f, 0.6f })
    {
        const auto mix = makeMix (guitar, sr, pan);
        const auto iso = isolateGuitar (mix.left.data(), mix.right.data(), mix.left.size(), sr, Focus::automatic);
        CHECK (iso.ok);
        CHECK_MSG (iso.pan * pan > 0.0f, "found pan %.2f for guitar at %.2f", iso.pan, pan);

        double num = 0, d1 = 0, d2 = 0;
        for (size_t i = 0; i < guitar.size(); ++i)
        {
            num += (double) iso.guitar[i] * guitar[i];
            d1 += (double) iso.guitar[i] * iso.guitar[i];
            d2 += (double) guitar[i] * guitar[i];
        }
        const double corr = std::abs (num) / std::sqrt (d1 * d2);
        CHECK_MSG (corr > 0.9, "pan %.1f: correlation with the real guitar %.2f", pan, corr);

        const auto f = analyzeTone (iso.guitar.data(), iso.guitar.size(), sr);
        float mx = 0.0f;
        const float err = spectrumError (f.ltasDb, truth.ltasDb, iso.validLoHz, iso.validHiHz, &mx);
        CHECK_MSG (err < 1.8f && mx < 5.0f, "pan %.1f: spectrum error mean %.2f max %.2f dB", pan, err, mx);

        // ...and far better than just using the mix
        Signal mono (mix.left.size());
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] = 0.5f * (mix.left[i] + mix.right[i]);
        const auto fm = analyzeTone (mono.data(), mono.size(), sr);
        CHECK (spectrumError (fm.ltasDb, truth.ltasDb, iso.validLoHz, iso.validHiHz) > 2.0f * err);
    }

    // cancellation works
    const auto mix = makeMix (guitar, sr, -0.5f);
    int calls = 0;
    const auto iso = isolateGuitar (mix.left.data(), mix.right.data(), mix.left.size(), sr, Focus::automatic,
                                    [&] (float) { return ++calls < 3; });
    CHECK (! iso.ok);
}

void testFitter()
{
    const double sr = 44100.0;
    ToneFitter fitter;

    // Different playing, an amp that is not the plugin's own model:
    // the fitted gain has to follow the amp's gain.
    for (int phrase : { 0, 1, 2 })
    {
        float fitted[3] = {};
        int i = 0;
        for (float ampGain : { 0.0f, 24.0f, 48.0f })
        {
            const auto ref = ampedReference (phrase, ampGain, sr);
            ToneProfile p;
            p.reference = analyzeTone (ref.data(), ref.size(), sr);
            p.targetDb = p.reference.ltasDb;
            CHECK (fitter.fit (p));
            fitted[i++] = DriveStage::gainDbFor (p.voicing, p.drive);
        }

        CHECK_MSG (fitted[0] < 10.0f, "phrase %d: clean reference fitted with %.0f dB of gain", phrase, fitted[0]);
        CHECK_MSG (fitted[2] > 30.0f, "phrase %d: high-gain reference fitted with %.0f dB of gain", phrase, fitted[2]);
        CHECK_MSG (fitted[0] < fitted[1] && fitted[1] < fitted[2] + 3.0f, "phrase %d: gains %.0f / %.0f / %.0f", phrase, fitted[0], fitted[1], fitted[2]);
    }

    // The plugin's own drive stage must be recovered closely.
    for (float drive : { 0.3f, 0.7f })
    {
        TestAmp amp;
        amp.useEngineDrive = true;
        amp.voicing = voicingCrunch;
        amp.drive = drive;
        const auto ref = amp.process (cleanGuitar (-1, sr, {}, 2), sr);
        ToneProfile p;
        p.reference = analyzeTone (ref.data(), ref.size(), sr);
        p.targetDb = p.reference.ltasDb;
        CHECK (fitter.fit (p));
        // (gain figures are only comparable within one voicing)
        const float want = DriveStage::gainDbFor (voicingCrunch, drive), got = DriveStage::gainDbFor (p.voicing, p.drive);
        CHECK_MSG (p.voicing != voicingCrunch || std::abs (want - got) < 8.0f, "own drive %.1f: wanted %.0f dB, fitted %.0f dB", drive, want, got);
        CHECK_MSG (p.fitError < 0.5f, "own drive %.1f: fit error %.2f", drive, p.fitError);
    }
}

void testToneConsistency()
{
    // The headline feature: whatever clean guitar goes in, at whatever level,
    // the same tone comes out.
    const double sr = 48000.0;
    const auto ref = ampedReference (0, 30.0f, 44100.0);
    const auto res = analyzeReference (ref.data(), nullptr, ref.size(), 44100.0, {});
    CHECK (res.ok);
    CHECK (! res.profile.fromFullMix);

    GuitarVoice dark, bright;
    dark.pickupHz = 2500.0f;
    dark.pickupQ = 1.1f;
    dark.pickBrightness = 0.3f;
    dark.seed = 3;
    bright.pickupHz = 6000.0f;
    bright.pickupQ = 2.5f;
    bright.pickBrightness = 0.9f;
    bright.seed = 4;

    struct Case
    {
        const char* name;
        GuitarVoice voice;
        float levelDb;
        int phrase;
    };

    const Case cases[] = {
        { "calibration guitar, nominal level", {}, -20.0f, -1 },
        { "very quiet input", {}, -40.0f, 2 },
        { "very hot input", {}, -6.0f, 0 },
        { "dark humbucker", dark, -26.0f, 0 },
        { "dark humbucker, chords", dark, -12.0f, 2 },
        { "bright single coil", bright, -30.0f, 0 },
        { "bright single coil, funk", bright, -16.0f, 3 },
    };

    float loudMin = 1.0e9f, loudMax = -1.0e9f;

    for (const auto& c : cases)
    {
        auto di = cleanGuitar (c.phrase, sr, c.voice, 4);
        normalise (di, c.levelDb);

        ToneEngine engine;
        engine.setSynchronous (true);
        engine.prepare (sr);
        applyProfile (engine, res.profile);

        const auto out = runEngine (engine, di);
        CHECK (allFinite (out));

        Signal tail (out.begin() + (std::ptrdiff_t) (out.size() / 2), out.end());
        const auto f = analyzeTone (tail.data(), tail.size(), sr);

        float mx = 0.0f;
        const float err = spectrumError (f.ltasDb, res.profile.targetDb, 100.0f, 5000.0f, &mx);
        CHECK_MSG (err < 2.0f && mx < 6.5f, "%s: spectrum error mean %.2f max %.2f dB", c.name, err, mx);

        const float loud = rmsDb (tail);
        loudMin = std::min (loudMin, loud);
        loudMax = std::max (loudMax, loud);

        // The saturation character must be consistent too, not just the EQ.
        if (c.phrase == 0)
            CHECK_MSG (std::abs (f.crestDb - res.profile.reference.crestDb) < 1.5f, "%s: crest %.1f vs reference %.1f",
                       c.name, f.crestDb, res.profile.reference.crestDb);
    }

    CHECK_MSG (loudMax - loudMin < 2.0f, "output loudness varies by %.1f dB across inputs spanning 34 dB", loudMax - loudMin);

    // Without the profile's adaptive stages a different guitar is clearly further off.
    {
        auto di = cleanGuitar (0, sr, dark, 4);
        normalise (di, -34.0f);

        float errors[2] = {};
        for (int adapt = 0; adapt < 2; ++adapt)
        {
            ToneEngine engine;
            engine.setSynchronous (true);
            engine.prepare (sr);
            applyProfile (engine, res.profile, adapt != 0);
            const auto out = runEngine (engine, di);
            Signal tail (out.begin() + (std::ptrdiff_t) (out.size() / 2), out.end());
            errors[adapt] = spectrumError (analyzeTone (tail.data(), tail.size(), sr).ltasDb, res.profile.targetDb, 100.0f, 5000.0f);
        }
        CHECK_MSG (errors[1] < errors[0], "adaptive %.2f dB vs frozen %.2f dB", errors[1], errors[0]);
    }
}

void testAdaptStateRoundTrip()
{
    // What the engine learned can be saved and restored (plugin state).
    const double sr = 48000.0;
    const auto ref = ampedReference (2, 20.0f, 44100.0);
    const auto res = analyzeReference (ref.data(), nullptr, ref.size(), 44100.0, {});
    CHECK (res.ok);

    auto di = cleanGuitar (2, sr, {}, 3);
    normalise (di, -35.0f);

    ToneEngine a;
    a.setSynchronous (true);
    a.prepare (sr);
    applyProfile (a, res.profile);
    runEngine (a, di);
    const auto state = a.getAdaptState();
    CHECK (state.valid);
    CHECK_MSG (std::abs (a.inputGainDb() - 15.0f) < 6.0f, "learned trim %.1f dB for a -35 dB guitar", a.inputGainDb());

    ToneEngine b;
    b.setSynchronous (true);
    b.prepare (sr);
    applyProfile (b, res.profile, false); // frozen: must use the restored state as-is
    b.setAdaptState (state);

    Signal shortDi (di.begin(), di.begin() + (std::ptrdiff_t) (2 * sr));
    const auto out = runEngine (b, shortDi);
    CHECK_MSG (std::abs (b.inputGainDb() - a.inputGainDb()) < 0.01f, "restored trim %.2f vs %.2f", b.inputGainDb(), a.inputGainDb());
    CHECK_MSG (std::abs (rmsDb (out) - (-20.0f)) < 4.0f, "restored engine output level %.1f dB", rmsDb (out));
}

void testReferencePipeline()
{
    const double sr = 44100.0;
    const auto guitar = ampedReference (0, 36.0f, sr, 3);

    // 1. full mix, guitar panned: must be recognised, isolated and fitted like the bare guitar
    {
        const auto bare = analyzeReference (guitar.data(), nullptr, guitar.size(), sr, {});
        CHECK (bare.ok);

        const auto mix = makeMix (guitar, sr, -0.6f);
        const auto res = analyzeReference (mix.left.data(), mix.right.data(), mix.left.size(), sr, {});
        CHECK_MSG (res.ok, "%s", res.message.c_str());
        CHECK (res.profile.fromFullMix && res.profile.isolated && ! res.profile.usedExposedGuitar);
        CHECK (res.profile.isolationPan < 0.0f);
        CHECK (res.guitar.size() == guitar.size());

        const float err = spectrumError (res.profile.targetDb, bare.profile.targetDb, res.profile.validLoHz, 5000.0f);
        CHECK_MSG (err < 1.8f, "target from mix vs bare guitar: %.2f dB", err);

        const float gBare = DriveStage::gainDbFor (bare.profile.voicing, bare.profile.drive);
        const float gMix = DriveStage::gainDbFor (res.profile.voicing, res.profile.drive);
        CHECK_MSG (std::abs (gBare - gMix) < 12.0f, "gain from mix %.0f dB vs bare %.0f dB", gMix, gBare);
    }

    // 2. song with a guitar intro: the exposed guitar is found and used directly
    {
        const size_t intro = (size_t) (9.0 * sr);
        auto mix = makeMix (guitar, sr, 0.0f);
        Signal g = guitar;
        normalise (g, -20.0f);
        for (size_t i = 0; i < intro; ++i)
            mix.left[i] = mix.right[i] = g[i];

        const auto res = analyzeReference (mix.left.data(), mix.right.data(), mix.left.size(), sr, {});
        CHECK_MSG (res.ok, "%s", res.message.c_str());
        CHECK (res.profile.fromFullMix);
        CHECK_MSG (res.profile.usedExposedGuitar && ! res.profile.isolated, "exposed intro not used");
        CHECK_MSG (res.profile.usedEndSec <= 9.5, "used %.1f..%.1f s", res.profile.usedStartSec, res.profile.usedEndSec);
    }

    // 3. user overrides and regions
    {
        const auto mix = makeMix (guitar, sr, 0.5f);
        ReferenceOptions opt;
        opt.mode = SourceMode::guitarOnly;
        opt.regionStartSec = 2.0;
        opt.regionEndSec = 8.0;
        const auto res = analyzeReference (mix.left.data(), mix.right.data(), mix.left.size(), sr, opt);
        CHECK (res.ok && ! res.profile.fromFullMix && ! res.profile.isolated);
        CHECK (std::abs ((double) res.guitar.size() - 6.0 * sr) < 2.0);

        opt.mode = SourceMode::fullMix;
        opt.focus = Focus::right;
        const auto res2 = analyzeReference (mix.left.data(), mix.right.data(), mix.left.size(), sr, opt);
        CHECK (res2.ok && res2.profile.isolated && res2.profile.isolationPan > 0.0f);
    }

    // 4. unusable input fails cleanly
    {
        Signal silence ((size_t) (5 * sr), 0.0f);
        const auto res = analyzeReference (silence.data(), nullptr, silence.size(), sr, {});
        CHECK (! res.ok && ! res.message.empty());

        Signal tiny (1000, 0.1f);
        CHECK (! analyzeReference (tiny.data(), nullptr, tiny.size(), sr, {}).ok);
    }
}

void testReverbEstimate()
{
    const double sr = 44100.0;

    // staccato chords, dry vs. with the engine's own reverb
    double len = 0.0;
    std::vector<NoteEvent> ev;
    for (int i = 0; i < 10; ++i)
        ev.push_back (makeNote (0.1 + i * 1.0, 0.3, 0.9f, { 45, 52, 57 }));
    len = 10.5;
    auto dry = renderGuitar (ev, sr, len, {});

    float mix = -1.0f, decay = 0.0f;
    CHECK (estimateReverb (dry.data(), dry.size(), sr, mix, decay));
    CHECK_MSG (mix < 0.03f, "dry signal estimated with %.2f reverb", mix);

    Reverb rev;
    rev.prepare (sr);
    rev.set (0.35f, 1.8f);
    rev.prepare (sr);
    Signal wetL (dry.size()), wetR (dry.size());
    for (size_t pos = 0; pos < dry.size(); pos += 64)
        rev.process (dry.data() + pos, wetL.data() + pos, wetR.data() + pos, (int) std::min ((size_t) 64, dry.size() - pos));

    CHECK (estimateReverb (wetL.data(), wetL.size(), sr, mix, decay));
    CHECK_MSG (mix > 0.08f, "reverberant signal estimated with %.2f reverb", mix);
    CHECK_MSG (decay > 0.8f && decay < 4.0f, "decay %.2f s for a 1.8 s reverb", decay);
}

void testWavIo()
{
    WavData w;
    w.sampleRate = 48000.0;
    w.channels.assign (2, Signal (1000));
    for (size_t i = 0; i < 1000; ++i)
    {
        w.channels[0][i] = std::sin ((float) i * 0.05f) * 0.8f;
        w.channels[1][i] = -0.5f + (float) i / 1000.0f;
    }

    std::string error;
    const std::string path = "tonegenie_test_tmp.wav";
    CHECK (writeWav (path, w, error));

    WavData r;
    CHECK (readWav (path, r, error));
    CHECK (r.sampleRate == 48000.0 && r.channels.size() == 2 && r.numSamples() == 1000);

    float err = 0.0f;
    for (size_t i = 0; i < 1000 && r.numSamples() == 1000; ++i)
        err = std::max ({ err, std::abs (r.channels[0][i] - w.channels[0][i]), std::abs (r.channels[1][i] - w.channels[1][i]) });
    CHECK_MSG (err < 1.0e-6f, "24-bit round trip error %g", err);

    std::remove (path.c_str());
    CHECK (! readWav ("does_not_exist.wav", r, error));
}

} // namespace

int main (int argc, char** argv)
{
    struct Test
    {
        const char* name;
        void (*fn)();
    };

    const Test tests[] = {
        { "fft", testFft },
        { "bands", testBands },
        { "min-phase-design", testMinPhaseDesign },
        { "oversampler", testOversampler },
        { "wav-io", testWavIo },
        { "engine-basics", testEngineBasics },
        { "engine-robustness", testEngineRobustness },
        { "classifier", testClassifier },
        { "isolation", testIsolation },
        { "reverb-estimate", testReverbEstimate },
        { "fitter", testFitter },
        { "reference-pipeline", testReferencePipeline },
        { "tone-consistency", testToneConsistency },
        { "adapt-state", testAdaptStateRoundTrip },
        { "background-design", testBackgroundDesign },
    };

    const std::string filter = argc > 1 ? argv[1] : "";

    for (const auto& t : tests)
    {
        if (! filter.empty() && std::string (t.name).find (filter) == std::string::npos)
            continue;

        const int before = failures;
        const auto start = std::chrono::steady_clock::now();
        t.fn();
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::printf ("%-22s %s  (%.1f s)\n", t.name, failures == before ? "ok" : "FAILED", secs);
        std::fflush (stdout);
    }

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
