#pragma once

#include "Bands.h"

namespace tg
{

/** Measurable characteristics of a guitar signal. The same code produces these
    for the reference recording and for candidate pedal settings, so the two can
    be compared like-for-like. */
struct ToneFeatures
{
    BandArray ltasDb {};          ///< long-term average spectrum (power density, dB)
    float crestDb = 0.0f;         ///< short-term peak/RMS (lower quartile of the loud frames): low = clipped waveform
    float density = 0.0f;         ///< spectral density of those frames (single notes: low, chords: high)
    float sustainDbPerSec = 0.0f; ///< how fast held notes decay (negative): near zero = saturated/compressed
    bool sustainValid = false;    ///< false if the material had no held notes to measure
    float levelSpreadDb = 0.0f;   ///< 10th..90th percentile spread of frame level: low = compressed
    float loudDb = -100.0f;       ///< 95th percentile frame level
    float activeSeconds = 0.0f;   ///< amount of signal that was actually analysed
};

enum Voicing
{
    voicingSmooth = 0,   ///< soft, mid-focused overdrive
    voicingCrunch = 1,   ///< two-stage amp-style drive
    voicingFuzz = 2,     ///< hard, asymmetric, full-range
    numVoicings
};

/** Everything the effect needs to reproduce a reference tone. */
struct ToneProfile
{
    bool valid = false;

    // --- spectral target -----------------------------------------------------
    BandArray targetDb {};        ///< reference guitar LTAS
    BandArray expectedPreDb {};   ///< LTAS the drive stage produces for the calibration guitar
    float validLoHz = 60.0f;      ///< range in which targetDb is trustworthy
    float validHiHz = 16000.0f;
    float canonicalTiltDb = 0.0f; ///< brightness of the calibration guitar (for pickup compensation)

    // --- fitted pedal settings ----------------------------------------------
    int voicing = voicingCrunch;
    float drive = 0.5f;           ///< 0..1
    float comp = 0.0f;            ///< 0..1
    float gate = 0.2f;            ///< 0..1
    float reverbMix = 0.0f;       ///< 0..1
    float reverbDecay = 1.2f;     ///< seconds
    float fitError = 0.0f;

    // --- how the reference was obtained --------------------------------------
    ToneFeatures reference;
    bool fromFullMix = false;     ///< file was classified / treated as a full mix
    bool usedExposedGuitar = false; ///< tone taken from passages where the guitar plays alone
    bool isolated = false;        ///< guitar was extracted by source separation
    float mixProbability = 0.0f;  ///< classifier confidence that the file is a full mix
    float isolationPan = 0.0f;    ///< -1 (left) .. +1 (right) position that was extracted
    double usedStartSec = 0.0, usedEndSec = 0.0;
};

} // namespace tg
