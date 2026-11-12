#include "tonegenie/SyntheticGuitar.h"
#include "tonegenie/DspBlocks.h"

#include <cmath>

namespace tg
{

NoteEvent makeNote (double time, double duration, float velocity, std::initializer_list<int> midiNotes, bool palmMute)
{
    NoteEvent e;
    e.time = time;
    e.duration = duration;
    e.velocity = velocity;
    e.palmMute = palmMute;
    for (int m : midiNotes)
        if (e.numNotes < 6)
            e.midi[e.numNotes++] = m;
    return e;
}

static void renderString (std::vector<float>& out, double sr, double startTime, double duration,
                          double f0, float velocity, bool palmMute, const GuitarVoice& voice, Rng& rng)
{
    const double period = sr / f0;
    const int len = std::max (2, (int) std::floor (period - 0.5));
    const double frac = period - 0.5 - len;
    const float apCoeff = (float) ((1.0 - frac) / (1.0 + frac));

    // Excitation: filtered noise burst shaped by the pick position.
    std::vector<float> buf ((size_t) len);
    OnePole pickLp;
    pickLp.setCutoff (1200.0 + 7000.0 * voice.pickBrightness * (0.5 + 0.5 * velocity), sr);

    for (auto& v : buf)
        v = pickLp.lp (rng.next());
    for (auto& v : buf) // second pass removes the filter's start-up transient character
        v = pickLp.lp (v);

    const int pickDelay = std::max (1, (int) std::lround (voice.pickPosition * len));
    std::vector<float> exc (buf);
    for (int i = 0; i < len; ++i)
        buf[(size_t) i] = exc[(size_t) i] - exc[(size_t) ((i + len - pickDelay) % len)];

    float mean = 0.0f, peak = 1.0e-9f;
    for (float v : buf)
        mean += v;
    mean /= (float) len;
    for (auto& v : buf)
    {
        v -= mean;
        peak = std::max (peak, std::abs (v));
    }
    for (auto& v : buf)
        v *= 0.3f * velocity / peak;

    const double t60 = palmMute ? 0.22 : std::clamp (6.0 - 2.2 * std::log2 (f0 / 82.0), 1.5, 6.0);
    const float gSustain = (float) std::pow (10.0, -3.0 / (f0 * t60));
    const float gRelease = (float) std::pow (10.0, -3.0 / (f0 * 0.05));

    const auto start = (long long) std::llround (startTime * sr);
    const auto noteOff = (long long) std::llround (duration * sr);
    const auto maxLen = noteOff + (long long) (0.35 * sr);

    int idx = 0;
    float prev = 0.0f, apX = 0.0f, apY = 0.0f;

    for (long long n = 0; n < maxLen; ++n)
    {
        const long long o = start + n;
        if (o >= (long long) out.size())
            break;

        const float y = buf[(size_t) idx];
        if (o >= 0)
            out[(size_t) o] += y;

        const float g = n < noteOff ? gSustain : gRelease;
        const float damp = palmMute ? 0.5f : 0.5f - 0.2f * voice.pickBrightness * 0.5f;
        const float avg = g * ((1.0f - damp) * y + damp * prev);
        prev = y;

        // first-order all-pass for fractional tuning
        const float ap = apCoeff * (avg - apY) + apX;
        apX = avg;
        apY = ap;

        buf[(size_t) idx] = ap;
        if (++idx >= len)
            idx = 0;
    }
}

std::vector<float> renderGuitar (const std::vector<NoteEvent>& events, double sr, double lengthSeconds, const GuitarVoice& voice)
{
    std::vector<float> out ((size_t) (lengthSeconds * sr), 0.0f);
    Rng rng (voice.seed);

    for (const auto& e : events)
        for (int i = 0; i < e.numNotes; ++i)
        {
            const double f0 = 440.0 * std::pow (2.0, (e.midi[i] - 69) / 12.0);
            renderString (out, sr, e.time + 0.009 * i, e.duration, f0, e.velocity, e.palmMute, voice, rng);
        }

    Biquad pickup;
    pickup.setLowPass (voice.pickupHz, voice.pickupQ, sr);
    OnePole hp;
    hp.setCutoff (70.0, sr);

    float peak = 1.0e-9f;
    for (auto& v : out)
    {
        v = hp.hp (pickup.process (v));
        peak = std::max (peak, std::abs (v));
    }
    for (auto& v : out)
        v *= 0.5f / peak;

    return out;
}

namespace
{
    constexpr int E2 = 40, G2 = 43, A2 = 45, C3 = 48, D3 = 50, E3 = 52, G3 = 55, A3 = 57, B3 = 59,
                  C4 = 60, D4 = 62, E4 = 64, G4 = 67, A4 = 69, B4 = 71, D5 = 74, E5 = 76;

    void power (std::vector<NoteEvent>& ev, double t, double dur, int root, float vel, bool palm)
    {
        ev.push_back (makeNote (t, dur, vel, { root, root + 7, root + 12 }, palm));
    }
}

std::vector<NoteEvent> calibrationPhrase (double& lengthSeconds)
{
    std::vector<NoteEvent> ev;
    double t = 0.05;

    // riff: palm mutes and open power chords
    for (int root : { E2, G2, A2 })
    {
        power (ev, t, 0.16, root, 0.9f, true);
        power (ev, t + 0.2, 0.16, root, 0.8f, true);
        power (ev, t + 0.4, 0.65, root, 1.0f, false);
        t += 1.1;
    }

    // open chords
    ev.push_back (makeNote (t, 0.8, 0.9f, { E2, 47, E3, G3, B3, E4 }));
    t += 0.85;
    ev.push_back (makeNote (t, 0.8, 0.8f, { C3, E3, G3, C4, E4 }));
    t += 0.85;
    ev.push_back (makeNote (t, 0.8, 0.85f, { D3, A3, D4, 66 }));
    t += 0.85;

    // arpeggio
    for (int m : { E3, G3, B3, E4, A3, C4 })
    {
        ev.push_back (makeNote (t, 0.26, 0.75f, { m }));
        t += 0.22;
    }

    // lead: long held notes
    for (int m : { A4, B4, D5, G4 })
    {
        ev.push_back (makeNote (t, 0.7, 0.9f, { m }));
        t += 0.75;
    }
    ev.push_back (makeNote (t, 1.0, 1.0f, { E5 }));
    t += 1.1;

    lengthSeconds = t + 0.2;
    return ev;
}

std::vector<NoteEvent> testPhrase (int which, double& lengthSeconds)
{
    std::vector<NoteEvent> ev;
    double t = 0.05;

    switch (which)
    {
        case 0: // power chord riff
            for (int rep = 0; rep < 2; ++rep)
                for (int root : { A2, C3, D3, A2 })
                {
                    power (ev, t, 0.15, root, 0.9f, true);
                    power (ev, t + 0.2, 0.15, root, 0.85f, true);
                    power (ev, t + 0.4, 0.35, root, 1.0f, false);
                    power (ev, t + 0.8, 0.55, root + 2, 0.95f, false);
                    t += 1.4;
                }
            break;

        case 1: // lead line
            for (int m : { A4, B4, D5, E5, D5, B4, A4, G4, A4, E5 })
            {
                ev.push_back (makeNote (t, 0.75, 0.9f, { m }));
                t += 0.8;
            }
            break;

        case 2: // strummed open chords
            for (int rep = 0; rep < 2; ++rep)
            {
                ev.push_back (makeNote (t, 1.1, 0.9f, { A2, E3, A3, C4, E4 }));
                t += 1.2;
                ev.push_back (makeNote (t, 1.1, 0.8f, { 41, C3, 53, A3, C4 }));
                t += 1.2;
                ev.push_back (makeNote (t, 1.1, 0.85f, { C3, E3, G3, C4, E4 }));
                t += 1.2;
                ev.push_back (makeNote (t, 1.1, 0.9f, { G2, 47, D3, G3, B3, G4 }));
                t += 1.2;
            }
            break;

        default: // funky clean stabs
            for (int rep = 0; rep < 12; ++rep)
            {
                ev.push_back (makeNote (t, 0.09, 0.9f, { E3 + 4, B3, D4, 66 }));
                ev.push_back (makeNote (t + 0.25, 0.07, 0.7f, { E3 + 4, B3, D4, 66 }));
                ev.push_back (makeNote (t + 0.5, 0.2, 0.8f, { rep % 2 ? E3 : G3 }));
                t += 0.75;
            }
            break;
    }

    lengthSeconds = t + 0.3;
    return ev;
}

} // namespace tg
