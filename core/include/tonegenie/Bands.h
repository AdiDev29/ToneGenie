#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace tg
{

/** The tone "fingerprint" grid: 1/6-octave bands from 40 Hz to ~18 kHz.
    Everything that describes a spectrum (target tone, live measurement,
    EQ curve) uses this layout so it is independent of sample rate and FFT size. */
constexpr int kNumBands = 54;
constexpr float kBandBaseHz = 40.0f;
constexpr int kBandsPerOctave = 6;

using BandArray = std::array<float, kNumBands>;

inline float bandCenterHz (int band) noexcept
{
    return kBandBaseHz * std::pow (2.0f, (float) band / (float) kBandsPerOctave);
}

/** Width in Hz of one band (edge to edge). */
inline float bandWidthHz (int band) noexcept
{
    static const float k = std::pow (2.0f, 0.5f / kBandsPerOctave) - std::pow (2.0f, -0.5f / kBandsPerOctave);
    return bandCenterHz (band) * k;
}

/** Fractional band index of a frequency. */
inline float bandIndexForHz (float hz) noexcept
{
    return (float) kBandsPerOctave * std::log2 (std::max (hz, 1.0f) / kBandBaseHz);
}

inline float powerToDb (float p) noexcept { return 10.0f * std::log10 (std::max (p, 1.0e-20f)); }
inline float dbToPower (float db) noexcept { return std::pow (10.0f, 0.1f * db); }
inline float dbToGain (float db) noexcept { return std::pow (10.0f, 0.05f * db); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-10f)); }

/** FFT size used for tone analysis: ~10.8 Hz resolution at any sample rate. */
int analysisFftSize (double sampleRate) noexcept;

/** Maps between FFT bins and the band grid for one (sampleRate, fftSize) pair. */
class BandMapper
{
public:
    void prepare (double sampleRate, int fftSize);

    /** power: fftSize/2+1 values (any consistent scaling). Output: mean power
        per bin inside each band. Bands above Nyquist are filled by extrapolating
        a steep roll-off from the last valid band. */
    void powerToBands (const float* power, float* bands) const noexcept;

    /** Interpolates per-band dB values onto FFT bins (fftSize/2+1 values). */
    void bandsToBins (const float* bandDb, float* binDb) const noexcept;

    int numBins() const noexcept { return bins; }
    int numValidBands() const noexcept { return validBands; }
    double sampleRate() const noexcept { return sr; }

private:
    struct Tap
    {
        int bin;
        float weight;
    };

    std::vector<Tap> taps;
    std::array<int, kNumBands + 1> offsets {};
    std::vector<int> binBand;
    std::vector<float> binFrac;
    int bins = 0, validBands = 0;
    double sr = 44100.0;
};

/** Triangular smoothing of a band curve over +/- halfWidth bands. */
void smoothBands (const BandArray& in, BandArray& out, int halfWidth) noexcept;

} // namespace tg
