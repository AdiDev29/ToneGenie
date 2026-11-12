#pragma once

#include <string>
#include <vector>

namespace tg
{

/** Minimal WAV reader/writer for the command-line tool and the tests.
    (The plugin itself uses JUCE's decoders and supports many more formats.) */
struct WavData
{
    double sampleRate = 44100.0;
    std::vector<std::vector<float>> channels;

    size_t numSamples() const { return channels.empty() ? 0 : channels.front().size(); }
};

bool readWav (const std::string& path, WavData& out, std::string& error);
bool writeWav (const std::string& path, const WavData& data, std::string& error);

} // namespace tg
