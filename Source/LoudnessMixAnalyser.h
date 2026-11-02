#pragma once

#include "MixAnalysis.h"
#include "oberli/Loudness.h"

/*  A B see - the mix meter: the shared `oberli::Loudness` behind the `MixAnalyser` seam.

    The meter is the family's (BS.1770-4 / EBU Tech 3341 and 3342 at every rate, tested by
    `oberli_loudness`); this file only maps its readings onto the contract `MixAnalysis.h`
    fixes, and adds nothing to the measurement:

      - `valid` once the meter has one full 400 ms gating block (its momentary reading exists);
      - integrated "since this section started": `reset()` clears the history (rule 17: the
        meter's own gating, never a second one here);
      - true peak is the meter's maximum since that reset, as the match policy wants it.

    `process` is the meter's own real-time-safe path; `prepare` allocates (D.1.5). */
namespace absee
{
class LoudnessMixAnalyser final : public MixAnalyser
{
public:
    void prepare (double sampleRate, int) override { meter.prepare (sampleRate, 2); }
    void reset() override { meter.reset(); }

    void process (const juce::AudioBuffer<float>& input, int numSamples) noexcept override
    {
        const int channels = juce::jmin (2, input.getNumChannels());
        if (channels > 0 && numSamples > 0)
            meter.process (input.getArrayOfReadPointers(), channels, numSamples);
    }

    MixReading read() const noexcept override
    {
        const auto r = meter.readings();
        MixReading m;
        m.valid = r.momentary > oberli::Loudness::kSilence;
        m.integratedLufs = r.integrated;      // kSilence (-1e9) reads as "no reading" in MatchEngine
        m.shortLufs = r.shortTerm;
        m.truePeakDb = r.truePeak;
        return m;
    }

private:
    oberli::Loudness meter;
};
} // namespace absee
