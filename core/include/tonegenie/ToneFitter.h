#pragma once

#include "Analysis.h"
#include "ToneProfile.h"

#include <vector>

namespace tg
{

/** Finds the pedal settings that make a clean guitar sound like the reference.

    This is analysis-by-synthesis: a model clean guitar (the "calibration
    guitar") is played through the real drive stage and matching EQ at many
    settings, the result is measured with exactly the same feature extractor
    that measured the reference, and the closest setting wins. */
class ToneFitter
{
public:
    static constexpr double kSampleRate = 44100.0;

    ToneFitter();

    struct Candidate
    {
        int voicing = voicingCrunch;
        float drive = 0.5f, comp = 0.0f;
        ToneFeatures features;     ///< of the pedal output (after matching EQ)
        BandArray preDb {};        ///< spectrum between drive stage and EQ
        float distance = 0.0f;
    };

    /** Renders the calibration guitar through one setting and measures it. */
    Candidate evaluate (int voicing, float drive, float comp, const ToneFeatures& reference,
                        float validLoHz, float validHiHz, float crestWeight = 1.0f,
                        std::vector<float>* renderedOutput = nullptr) const;

    /** Full search. Fills the fitted fields of `profile` (voicing, drive, comp,
        gate, expectedPreDb, canonicalTiltDb, fitError). targetDb / valid range /
        reference must already be set. Returns false if cancelled. */
    bool fit (ToneProfile& profile, const ProgressFn& progress = {}) const;

    static float distance (const ToneFeatures& candidate, const ToneFeatures& reference, float crestWeight) noexcept;

    float canonicalTiltDb() const noexcept { return canonicalTilt; }
    const std::vector<float>& calibrationSignal() const noexcept { return levelled; }

private:
    std::vector<float> levelled; ///< calibration guitar after input levelling
    float canonicalTilt = 0.0f;
};

} // namespace tg
