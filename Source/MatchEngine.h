#pragma once

#include "MixAnalysis.h"
#include "ReferenceStore.h"

#include <juce_core/juce_core.h>

#include <cmath>

/*  A B see - the match policy: two loudness readings in, two gains out.

    Hand port of `sound design/prototype/absee_proto.py` (class MatchEngine). This is A B
    see's own DSP; the readings come from the shared meter through `MixAnalysis.h` and from
    the section analysis `oberli::References` computed on load. Measured at G3:
    `proto_measure.py --case match-integrated|match-short|match-truepeak|match-target`. */
namespace absee
{
/** The Match parameter's choices, in the order of parameters.csv. */
enum class MatchMode { integrated = 0, shortTerm = 1, truePeak = 2, off = 3 };

/** The hysteresis of Integrated mode, in LU (brief 4.2: the gain only moves when the
    measurement moves by more than this). It is also the bound on the Integrated match
    error: G3 measured 0.070 LU against the brief's 0.1 LU target, and that number *is*
    this band, not noise. */
inline constexpr double kMatchHysteresisLu = 0.1;

class MatchEngine
{
public:
    /** `tauSeconds` is the brief's 20 ms: a changing short-term match must not pump. */
    void prepare (double sampleRate, double tauSeconds) noexcept
    {
        coeff = (float) std::exp (-1.0 / (juce::jmax (1.0e-6, tauSeconds) * sampleRate));
        reset();
    }

    void reset() noexcept
    {
        referenceDb = mixDb = 0.0f;
        referenceTarget = mixTarget = 0.0f;
        holdValid = false;
        heldMixLufs = 0.0;
    }

    /** The unsmoothed targets, once per block (A.0: what the readout prints comes from here,
        not from the smoothed path). */
    void setTargets (MatchMode mode, const MixReading& mix, const SectionAnalysis& section,
                     bool trimMixInstead, float matchTargetLufs) noexcept
    {
        referenceTarget = mixTarget = 0.0f;
        if (mode == MatchMode::off)
        {
            holdValid = false;
            return;
        }
        if (! mix.valid)
            return;

        const bool refLoudnessOk = section.isUsable() && section.integratedLufs > -200.0;
        const bool refPeakOk = section.isUsable() && section.truePeakDb > -200.0;

        if (trimMixInstead)
        {
            // Both sides are brought to the target, so a loud master and a quiet reference
            // meet at a sane monitoring level (parameters.csv: matchTarget).
            if (mode == MatchMode::truePeak)
            {
                if (refPeakOk && mix.truePeakDb > -200.0)
                {
                    referenceTarget = (float) (matchTargetLufs - section.truePeakDb);
                    mixTarget = (float) (matchTargetLufs - mix.truePeakDb);
                }
                return;
            }
            const double mixLufs = mode == MatchMode::integrated ? mix.integratedLufs : mix.shortLufs;
            if (refLoudnessOk && mixLufs > -200.0)
            {
                referenceTarget = (float) (matchTargetLufs - section.integratedLufs);
                mixTarget = (float) (matchTargetLufs - mixLufs);
            }
            return;
        }

        // The Listen switch's promise: the reference comes to the mix, the mix is untouched.
        if (mode == MatchMode::truePeak)
        {
            if (refPeakOk && mix.truePeakDb > -200.0)
                referenceTarget = (float) (mix.truePeakDb - section.truePeakDb);
            return;
        }
        if (! refLoudnessOk)
            return;
        double mixLufs = mode == MatchMode::integrated ? mix.integratedLufs : mix.shortLufs;
        if (mixLufs <= -200.0)
            return;
        if (mode == MatchMode::integrated)
        {
            if (! holdValid || std::abs (mixLufs - heldMixLufs) > kMatchHysteresisLu)
            {
                heldMixLufs = mixLufs;
                holdValid = true;
            }
            mixLufs = heldMixLufs;
        }
        referenceTarget = (float) (mixLufs - section.integratedLufs);
    }

    /** n samples of **linear** gain for each side, one-pole smoothed in the dB domain. */
    void advance (float* referenceGain, float* mixGain, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            referenceDb = referenceTarget + coeff * (referenceDb - referenceTarget);
            mixDb = mixTarget + coeff * (mixDb - mixTarget);
            referenceGain[i] = juce::Decibels::decibelsToGain (referenceDb);
            mixGain[i] = juce::Decibels::decibelsToGain (mixDb);
        }
    }

    float referenceTargetDb() const noexcept { return referenceTarget; }
    float mixTargetDb() const noexcept { return mixTarget; }

    /** True while the mix side carries no gain at all: both the target and the state are
        exactly 0 dB, so the mix path can be left untouched (the wire). */
    bool mixIsUntouched() const noexcept
    {
        return juce::exactlyEqual (mixTarget, 0.0f) && juce::exactlyEqual (mixDb, 0.0f);
    }

    /** Defining feature 2: what was done, to which side, in dB - never a bare number.
        Message thread (it builds a string). */
    juce::String readout() const
    {
        const bool mixSide = std::abs (mixTarget) > std::abs (referenceTarget);
        const float value = mixSide ? mixTarget : referenceTarget;
        return juce::String (mixSide ? "Mix " : "Reference ")
             + (value >= 0.0f ? "+" : "-") + juce::String (std::abs (value), 1) + " dB";
    }

private:
    float coeff = 0.0f;
    float referenceDb = 0.0f, mixDb = 0.0f;
    float referenceTarget = 0.0f, mixTarget = 0.0f;
    double heldMixLufs = 0.0;
    bool holdValid = false;
};
} // namespace absee
