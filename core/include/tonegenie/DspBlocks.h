#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace tg
{

constexpr float kPi = 3.14159265358979323846f;

/** Largest block the internal processors are ever asked to handle at once. */
constexpr int kMaxChunk = 64;

inline float onePoleCoeff (double timeSeconds, double sampleRate) noexcept
{
    return timeSeconds <= 0.0 ? 1.0f : (float) (1.0 - std::exp (-1.0 / (timeSeconds * sampleRate)));
}

/** One-pole low-pass / high-pass. */
struct OnePole
{
    float a = 1.0f, z = 0.0f;

    void setCutoff (double hz, double sampleRate) noexcept
    {
        a = (float) (1.0 - std::exp (-2.0 * 3.14159265358979323846 * std::min (hz, 0.49 * sampleRate) / sampleRate));
    }

    void reset() noexcept { z = 0.0f; }
    float lp (float x) noexcept { z += a * (x - z); return z; }
    float hp (float x) noexcept { z += a * (x - z); return x - z; }
};

/** RBJ biquad, transposed direct form II. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0.0f; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = (float) (nb0 / na0);
        b1 = (float) (nb1 / na0);
        b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0);
        a2 = (float) (na2 / na0);
    }

    void setLowPass (double hz, double q, double sr) noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setHighPass (double hz, double q, double sr) noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void setBandPass (double hz, double q, double sr) noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set (al, 0, -al, 1 + al, -2 * c, 1 - al);
    }

    void setPeak (double hz, double q, double gainDb, double sr) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    void setHighShelf (double hz, double gainDb, double sr) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr;
        const double c = std::cos (w), s = std::sin (w);
        const double al = s / 2.0 * std::sqrt (2.0); // shelf slope S = 1
        const double sq = 2.0 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + sq),
             -2 * A * ((A - 1) + (A + 1) * c),
             A * ((A + 1) + (A - 1) * c - sq),
             (A + 1) - (A - 1) * c + sq,
             2 * ((A - 1) - (A + 1) * c),
             (A + 1) - (A - 1) * c - sq);
    }
};

/** Simple circular delay line with linear-interpolated read. */
class DelayLine
{
public:
    void prepare (int maxSamples)
    {
        int size = 1;
        while (size < maxSamples + 4)
            size <<= 1;
        buf.assign ((size_t) size, 0.0f);
        mask = size - 1;
        pos = 0;
    }

    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); }

    void push (float x) noexcept
    {
        buf[(size_t) pos] = x;
        pos = (pos + 1) & mask;
    }

    /** Sample written `delay` pushes ago (delay >= 1). */
    float read (int delay) const noexcept { return buf[(size_t) ((pos - delay) & mask)]; }

    float readInterp (float delay) const noexcept
    {
        const int d = (int) delay;
        const float f = delay - (float) d;
        const float a = buf[(size_t) ((pos - d) & mask)];
        const float b = buf[(size_t) ((pos - d - 1) & mask)];
        return a + f * (b - a);
    }

    int capacity() const noexcept { return mask - 3; }

private:
    std::vector<float> buf;
    int mask = 0, pos = 0;
};

/** Cheap deterministic noise source. */
struct Rng
{
    unsigned int state = 0x1234567u;

    explicit Rng (unsigned int seed = 1) : state (seed * 2654435761u + 0x9e3779b9u)
    {
        if (state == 0)
            state = 1;
    }

    unsigned int nextInt() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** Uniform in [-1, 1). */
    float next() noexcept { return (float) (nextInt() >> 8) * (2.0f / 16777216.0f) - 1.0f; }

    /** Uniform in [0, 1). */
    float next01() noexcept { return (float) (nextInt() >> 8) * (1.0f / 16777216.0f); }
};

} // namespace tg
