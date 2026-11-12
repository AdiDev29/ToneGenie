// ToneGenie command-line tool: the same analysis and processing as the plugin,
// for batch use and for testing without a DAW. WAV files only.

#include "WavIo.h"
#include "tonegenie/Reference.h"
#include "tonegenie/ToneEngine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

void usage()
{
    std::puts (
        "ToneGenie command-line tool\n"
        "\n"
        "  tonegenie-cli analyze <reference.wav> [options]\n"
        "      Detects whether the file is a lone guitar or a full mix, isolates the\n"
        "      guitar if needed and prints the tone that was found.\n"
        "\n"
        "  tonegenie-cli process <reference.wav> <clean-guitar.wav> <output.wav> [options]\n"
        "      Learns the tone of the reference and plays the clean guitar through it.\n"
        "\n"
        "options:\n"
        "  --mode auto|guitar|mix         how to treat the reference (default: auto)\n"
        "  --focus auto|left|center|right where the guitar sits in a full mix (default: auto)\n"
        "  --start <sec> --end <sec>      only use this part of the reference\n"
        "  --isolated <file.wav>          write the guitar signal the tone was taken from\n"
        "  --no-adapt                     do not adapt to the clean guitar while processing\n"
        "  --no-ambience                  disable reverb\n");
}

struct Args
{
    std::string command, reference, input, output, isolatedPath;
    tg::ReferenceOptions options;
    bool adapt = true, ambience = true;
};

bool parse (int argc, char** argv, Args& a)
{
    if (argc < 3)
        return false;

    a.command = argv[1];
    int i = 2;
    a.reference = argv[i++];

    if (a.command == "process")
    {
        if (argc < 5)
            return false;
        a.input = argv[i++];
        a.output = argv[i++];
    }
    else if (a.command != "analyze")
    {
        return false;
    }

    for (; i < argc; ++i)
    {
        const std::string opt = argv[i];
        const auto value = [&] () -> std::string { return i + 1 < argc ? argv[++i] : ""; };

        if (opt == "--mode")
        {
            const auto v = value();
            a.options.mode = v == "guitar" ? tg::SourceMode::guitarOnly : v == "mix" ? tg::SourceMode::fullMix : tg::SourceMode::automatic;
        }
        else if (opt == "--focus")
        {
            const auto v = value();
            a.options.focus = v == "left" ? tg::Focus::left : v == "right" ? tg::Focus::right
                            : v == "center" || v == "centre" ? tg::Focus::center : tg::Focus::automatic;
        }
        else if (opt == "--start")
            a.options.regionStartSec = std::atof (value().c_str());
        else if (opt == "--end")
            a.options.regionEndSec = std::atof (value().c_str());
        else if (opt == "--isolated")
            a.isolatedPath = value();
        else if (opt == "--no-adapt")
            a.adapt = false;
        else if (opt == "--no-ambience")
            a.ambience = false;
        else
            return false;
    }

    return true;
}

} // namespace

int main (int argc, char** argv)
{
    Args args;
    if (! parse (argc, argv, args))
    {
        usage();
        return 2;
    }

    std::string error;
    tg::WavData ref;
    if (! tg::readWav (args.reference, ref, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const float* left = ref.channels[0].data();
    const float* right = ref.channels.size() > 1 ? ref.channels[1].data() : nullptr;

    int lastPercent = -1;
    const auto result = tg::analyzeReference (left, right, ref.numSamples(), ref.sampleRate, args.options, [&] (float p)
    {
        const int percent = (int) (p * 100.0f);
        if (percent != lastPercent)
        {
            lastPercent = percent;
            std::fprintf (stderr, "\ranalysing... %3d%%", percent);
        }
        return true;
    });
    std::fprintf (stderr, "\r                    \r");

    if (! result.ok)
    {
        std::fprintf (stderr, "error: %s\n", result.message.c_str());
        return 1;
    }

    const auto& p = result.profile;
    static const char* const voices[] = { "Smooth", "Crunch", "Fuzz" };

    std::printf ("%s\n", result.message.c_str());
    std::printf ("  full-mix probability : %.0f%%\n", p.mixProbability * 100.0f);
    std::printf ("  analysed             : %.1f s .. %.1f s (%.1f s of guitar)\n", p.usedStartSec, p.usedEndSec, p.reference.activeSeconds);
    std::printf ("  voicing              : %s\n", voices[p.voicing]);
    std::printf ("  drive                : %.0f%%  (%.0f dB of gain)\n", p.drive * 100.0f, tg::DriveStage::gainDbFor (p.voicing, p.drive));
    std::printf ("  compression          : %.0f%%\n", p.comp * 100.0f);
    std::printf ("  gate                 : %.0f%%\n", p.gate * 100.0f);
    std::printf ("  reverb               : %.0f%% mix, %.1f s decay\n", p.reverbMix * 100.0f, p.reverbDecay);
    std::printf ("  reference character  : crest %.1f dB, sustain %.1f dB/s, level spread %.1f dB\n",
                 p.reference.crestDb, p.reference.sustainDbPerSec, p.reference.levelSpreadDb);

    if (! args.isolatedPath.empty())
    {
        tg::WavData iso;
        iso.sampleRate = result.guitarSampleRate;
        iso.channels.push_back (result.guitar);
        if (! tg::writeWav (args.isolatedPath, iso, error))
            std::fprintf (stderr, "warning: %s\n", error.c_str());
        else
            std::printf ("  wrote %s\n", args.isolatedPath.c_str());
    }

    if (args.command != "process")
        return 0;

    tg::WavData in;
    if (! tg::readWav (args.input, in, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return 1;
    }

    tg::ToneEngine engine;
    engine.setSynchronous (true);
    engine.prepare (in.sampleRate);

    tg::EngineParams params;
    params.voicing = p.voicing;
    params.drive = p.drive;
    params.comp = p.comp;
    params.gate = p.gate;
    params.adapt = args.adapt;
    params.reverbMix = args.ambience ? p.reverbMix : 0.0f;
    params.reverbDecay = p.reverbDecay;
    engine.setParams (params);
    engine.setProfile (&p);

    const size_t n = in.numSamples();
    const float* inL = in.channels[0].data();
    const float* inR = in.channels.size() > 1 ? in.channels[1].data() : nullptr;

    tg::WavData out;
    out.sampleRate = in.sampleRate;
    out.channels.assign (2, std::vector<float> (n));

    // First pass lets the engine learn the instrument, second pass is rendered
    // (in a DAW this happens while you play the first few bars).
    for (int pass = 0; pass < (args.adapt ? 2 : 1); ++pass)
        for (size_t pos = 0; pos < n; pos += 512)
        {
            const int count = (int) std::min ((size_t) 512, n - pos);
            engine.process (inL + pos, inR != nullptr ? inR + pos : nullptr,
                            out.channels[0].data() + pos, out.channels[1].data() + pos, count);
        }

    if (! tg::writeWav (args.output, out, error))
    {
        std::fprintf (stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::printf ("  input trim learned   : %+.1f dB, pickup compensation %+.1f dB\n", engine.inputGainDb(), engine.pickupCorrectionDb());
    std::printf ("  wrote %s\n", args.output.c_str());
    return 0;
}
