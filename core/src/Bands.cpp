#include "tonegenie/Bands.h"

#include <algorithm>

namespace tg
{

int analysisFftSize (double sampleRate) noexcept
{
    int n = 1024;
    while ((double) n < sampleRate / 16.0 && n < 32768)
        n <<= 1;
    return n; // 44.1k/48k -> 4096, 88.2k/96k -> 8192, 22.05k -> 2048
}

void BandMapper::prepare (double sampleRate, int fftSize)
{
    sr = sampleRate;
    bins = fftSize / 2 + 1;
    const double binHz = sampleRate / (double) fftSize;
    const double nyquistLimit = 0.47 * sampleRate;

    taps.clear();
    validBands = 0;

    for (int b = 0; b < kNumBands; ++b)
    {
        offsets[(size_t) b] = (int) taps.size();

        const double fc = bandCenterHz (b);
        if (fc > nyquistLimit)
            continue;

        validBands = b + 1;

        // Triangular window in log-frequency spanning the neighbouring band centres.
        const double fLo = bandCenterHz (b - 1), fHi = bandCenterHz (b + 1);
        const int kLo = std::max (1, (int) std::ceil (fLo / binHz));
        const int kHi = std::min (bins - 1, (int) std::floor (fHi / binHz));

        const auto first = taps.size();
        double sum = 0.0;

        for (int k = kLo; k <= kHi; ++k)
        {
            const double f = k * binHz;
            const double w = 1.0 - std::abs (std::log2 (f / fc)) * kBandsPerOctave;
            if (w > 0.0)
            {
                taps.push_back ({ k, (float) w });
                sum += w;
            }
        }

        if (taps.size() - first < 2)
        {
            // Band narrower than the bin spacing: interpolate between the two nearest bins.
            taps.resize (first);
            const double pos = std::clamp (fc / binHz, 1.0, (double) (bins - 2));
            const int k0 = (int) std::floor (pos);
            const float frac = (float) (pos - k0);
            taps.push_back ({ k0, 1.0f - frac });
            taps.push_back ({ k0 + 1, frac });
        }
        else
        {
            for (auto i = first; i < taps.size(); ++i)
                taps[i].weight = (float) (taps[i].weight / sum);
        }
    }

    for (int b = validBands; b <= kNumBands; ++b)
        offsets[(size_t) b] = (int) taps.size();

    binBand.assign ((size_t) bins, 0);
    binFrac.assign ((size_t) bins, 0.0f);

    for (int k = 0; k < bins; ++k)
    {
        const float pos = bandIndexForHz ((float) (k * binHz));
        if (pos <= 0.0f)
        {
            binBand[(size_t) k] = 0;
            binFrac[(size_t) k] = 0.0f;
        }
        else if (pos >= (float) (kNumBands - 1))
        {
            binBand[(size_t) k] = kNumBands - 2;
            binFrac[(size_t) k] = 1.0f;
        }
        else
        {
            const int b = (int) pos;
            binBand[(size_t) k] = b;
            binFrac[(size_t) k] = pos - (float) b;
        }
    }
}

void BandMapper::powerToBands (const float* power, float* bands) const noexcept
{
    for (int b = 0; b < validBands; ++b)
    {
        float acc = 0.0f;
        for (int i = offsets[(size_t) b]; i < offsets[(size_t) b + 1]; ++i)
            acc += power[taps[(size_t) i].bin] * taps[(size_t) i].weight;
        bands[b] = acc;
    }

    // Above Nyquist: continue with a steep (-24 dB/oct) roll-off so the values stay benign.
    const float rolloff = dbToPower (-24.0f / kBandsPerOctave);
    for (int b = validBands; b < kNumBands; ++b)
        bands[b] = (b > 0 ? bands[b - 1] : 1.0e-20f) * rolloff;
}

void BandMapper::bandsToBins (const float* bandDb, float* binDb) const noexcept
{
    for (int k = 0; k < bins; ++k)
    {
        const int b = binBand[(size_t) k];
        const float f = binFrac[(size_t) k];
        binDb[k] = bandDb[b] + f * (bandDb[b + 1] - bandDb[b]);
    }
}

void smoothBands (const BandArray& in, BandArray& out, int halfWidth) noexcept
{
    if (halfWidth <= 0)
    {
        out = in;
        return;
    }

    BandArray tmp {};
    for (int b = 0; b < kNumBands; ++b)
    {
        float acc = 0.0f, wsum = 0.0f;
        for (int d = -halfWidth; d <= halfWidth; ++d)
        {
            const int i = std::clamp (b + d, 0, kNumBands - 1);
            const float w = (float) (halfWidth + 1 - std::abs (d));
            acc += w * in[(size_t) i];
            wsum += w;
        }
        tmp[(size_t) b] = acc / wsum;
    }
    out = tmp;
}

} // namespace tg
