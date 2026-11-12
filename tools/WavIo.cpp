#include "WavIo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace tg
{

namespace
{
    template <typename T>
    T readLe (const unsigned char* p)
    {
        T v = 0;
        for (size_t i = 0; i < sizeof (T); ++i)
            v = (T) (v | ((T) p[i] << (8 * i)));
        return v;
    }

    template <typename T>
    void writeLe (std::ofstream& f, T v)
    {
        unsigned char b[sizeof (T)];
        for (size_t i = 0; i < sizeof (T); ++i)
            b[i] = (unsigned char) ((v >> (8 * i)) & 0xff);
        f.write ((const char*) b, sizeof (T));
    }
}

bool readWav (const std::string& path, WavData& out, std::string& error)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
    {
        error = "cannot open " + path;
        return false;
    }

    std::vector<unsigned char> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());

    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a RIFF/WAVE file: " + path;
        return false;
    }

    int format = 0, numChannels = 0, bits = 0;
    double sampleRate = 0.0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;

    for (size_t pos = 12; pos + 8 <= bytes.size();)
    {
        const auto size = (size_t) readLe<uint32_t> (&bytes[pos + 4]);
        const auto* body = &bytes[pos + 8];
        const size_t avail = std::min (size, bytes.size() - pos - 8);

        if (std::memcmp (&bytes[pos], "fmt ", 4) == 0 && avail >= 16)
        {
            format = readLe<uint16_t> (body);
            numChannels = readLe<uint16_t> (body + 2);
            sampleRate = readLe<uint32_t> (body + 4);
            bits = readLe<uint16_t> (body + 14);
            if (format == 0xfffe && avail >= 26) // WAVE_FORMAT_EXTENSIBLE
                format = readLe<uint16_t> (body + 24);
        }
        else if (std::memcmp (&bytes[pos], "data", 4) == 0)
        {
            data = body;
            dataSize = avail;
        }

        pos += 8 + size + (size & 1);
    }

    if (data == nullptr || numChannels <= 0 || sampleRate <= 0.0
        || ! ((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
    {
        error = "unsupported WAV encoding: " + path;
        return false;
    }

    const int bytesPer = bits / 8;
    const size_t frames = dataSize / (size_t) (bytesPer * numChannels);

    out.sampleRate = sampleRate;
    out.channels.assign ((size_t) numChannels, std::vector<float> (frames));

    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < numChannels; ++c)
        {
            const unsigned char* p = data + (i * (size_t) numChannels + (size_t) c) * (size_t) bytesPer;
            float v = 0.0f;

            if (format == 3)
            {
                const auto raw = readLe<uint32_t> (p);
                std::memcpy (&v, &raw, 4);
            }
            else if (bits == 8)
                v = ((float) p[0] - 128.0f) / 128.0f;
            else if (bits == 16)
                v = (float) (int16_t) readLe<uint16_t> (p) / 32768.0f;
            else if (bits == 24)
            {
                const auto raw = (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);
                v = (float) ((int32_t) (raw << 8) >> 8) / 8388608.0f;
            }
            else
                v = (float) ((double) (int32_t) readLe<uint32_t> (p) / 2147483648.0);

            out.channels[(size_t) c][i] = v;
        }

    return true;
}

bool writeWav (const std::string& path, const WavData& data, std::string& error)
{
    std::ofstream f (path, std::ios::binary);
    if (! f)
    {
        error = "cannot write " + path;
        return false;
    }

    const auto numChannels = (uint16_t) data.channels.size();
    const auto frames = (uint32_t) data.numSamples();
    const uint32_t dataBytes = frames * numChannels * 3u;

    f.write ("RIFF", 4);
    writeLe<uint32_t> (f, 36u + dataBytes);
    f.write ("WAVEfmt ", 8);
    writeLe<uint32_t> (f, 16);
    writeLe<uint16_t> (f, 1);
    writeLe<uint16_t> (f, numChannels);
    writeLe<uint32_t> (f, (uint32_t) std::lround (data.sampleRate));
    writeLe<uint32_t> (f, (uint32_t) std::lround (data.sampleRate) * numChannels * 3u);
    writeLe<uint16_t> (f, (uint16_t) (numChannels * 3));
    writeLe<uint16_t> (f, 24);
    f.write ("data", 4);
    writeLe<uint32_t> (f, dataBytes);

    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < numChannels; ++c)
        {
            const float v = std::clamp (data.channels[c][i], -1.0f, 1.0f);
            const auto s = (int32_t) std::lround ((double) v * 8388607.0);
            const unsigned char b[3] = { (unsigned char) (s & 0xff), (unsigned char) ((s >> 8) & 0xff), (unsigned char) ((s >> 16) & 0xff) };
            f.write ((const char*) b, 3);
        }

    return (bool) f;
}

} // namespace tg
