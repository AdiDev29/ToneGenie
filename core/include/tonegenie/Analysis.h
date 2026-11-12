#pragma once

#include "Bands.h"
#include "ToneProfile.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace tg
{

/** Progress callback: receives 0..1, returns false to cancel. */
using ProgressFn = std::function<bool (float)>;

/** Frame-wise spectral description of a mono signal. */
struct FrameAnalysis
{
    int fftSize = 0, hop = 0, numFrames = 0;
    double sampleRate = 0.0;
    std::vector<float> bandPsd;     ///< numFrames x kNumBands, power spectral density (units^2/Hz)
    std::vector<float> rmsDb;       ///< per frame
    std::vector<float> crestDb;     ///< per frame peak/RMS
    std::vector<float> density;     ///< per frame: log2 of the bandwidth (Hz) holding 85% of the 150 Hz..4 kHz energy
    std::vector<float> envDb;       ///< fine level envelope, one value every 10 ms
};

FrameAnalysis analyzeFrames (const float* x, size_t numSamples, double sampleRate);

/** Reduces a frame analysis to the tone features. */
ToneFeatures summarizeTone (const FrameAnalysis& frames);

/** Crest factor (lower quartile) of those loud frames whose spectral density is
    closest to `density` - i.e. "the crest this signal has for that kind of playing". */
float crestAtDensity (const FrameAnalysis& frames, float density);

/** Convenience: analyzeFrames + summarizeTone. */
ToneFeatures analyzeTone (const float* x, size_t numSamples, double sampleRate);

/** Rough reverb estimate from the tails after abrupt note stops.
    Returns false if nothing usable was found (outputs are then left at defaults). */
bool estimateReverb (const float* x, size_t numSamples, double sampleRate, float& mix, float& decaySeconds);

float percentile (std::vector<float> values, float p);

} // namespace tg
