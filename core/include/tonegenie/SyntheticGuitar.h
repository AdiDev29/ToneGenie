#pragma once

#include <initializer_list>
#include <vector>

namespace tg
{

/** A physically-inspired plucked string model (extended Karplus-Strong) that
    stands in for "a clean electric guitar" wherever the software needs one:
    as the calibration signal when fitting pedal settings to a reference tone,
    and as test material. */
struct GuitarVoice
{
    float pickupHz = 3800.0f;   ///< pickup/cable resonance
    float pickupQ = 1.6f;
    float pickBrightness = 0.6f; ///< 0 = soft fingers, 1 = hard pick
    float pickPosition = 0.18f;  ///< fraction of the string length
    unsigned int seed = 1;
};

struct NoteEvent
{
    double time = 0.0;      ///< seconds
    double duration = 0.5;  ///< seconds until the note is damped
    float velocity = 0.8f;
    bool palmMute = false;
    int numNotes = 0;
    int midi[6] {};
};

NoteEvent makeNote (double time, double duration, float velocity, std::initializer_list<int> midiNotes, bool palmMute = false);

std::vector<float> renderGuitar (const std::vector<NoteEvent>& events, double sampleRate, double lengthSeconds, const GuitarVoice& voice);

/** The fixed phrase used for calibration: riffs, open chords, arpeggios and lead notes. */
std::vector<NoteEvent> calibrationPhrase (double& lengthSeconds);

/** Alternative material (0: power-chord riff, 1: lead line, 2: strummed chords, 3: funky clean). */
std::vector<NoteEvent> testPhrase (int which, double& lengthSeconds);

} // namespace tg
