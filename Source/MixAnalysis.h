#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

/*  A B see - the seam the shared loudness meter plugs into.

    **This file deliberately contains no meter.** The BS.1770-5 streaming meter (per-rate
    K-weighting, gated integrated, 3 s short-term, 4x true peak, EBU Tech 3341/3342 test
    signals at every rate) is shared code: `oberli::Loudness` from Oberli-Audio/bob-skills,
    arriving in this repo as `Source/oberli/`. Writing a second one here would be the exact
    failure rule 17 and A.8 are about - two meters that disagree by half a dB and nobody
    knows which is right.

    What this file does is fix the *contract* A B see needs from it, so the port, the tests
    and the harness are written once:

      - `MixReading` is the measurement path, never the display path (A.0): unsmoothed,
        no peak hold, no decay. The editor smooths what it draws; the match gain uses this.
      - `MixAnalyser::process` runs on the audio thread, so it must do the few multiplies
        per sample of the K-weighting and the 100 ms sub-block accumulation, and nothing
        else: gating, percentiles and the display run off-thread (A.9 G1).
      - the processor holds `LoudnessMixAnalyser` (the shared meter, LoudnessMixAnalyser.h);
        `NoMixAnalyser` reads "no measurement", so with it the match gain stays at 0.0 dB on
        both sides and the readout says so - a *visible* gap, not a silent wrong number.
*/
namespace absee
{
/** One set of readings from the mix side's meter, all in the units the readout prints.
    `valid` is false until the meter has a full gating block (BS.1770: 400 ms). */
struct MixReading
{
    bool valid = false;
    double integratedLufs = -300.0;   ///< gated, since the section started (brief 4.2)
    double shortLufs = -300.0;        ///< the 3 s window ending now
    double truePeakDb = -400.0;       ///< dBTP, 4x oversampled (BS.1770 Annex 2)
};

/** The meter A B see measures the mix with. Implemented by `oberli::Loudness`. */
class MixAnalyser
{
public:
    virtual ~MixAnalyser() = default;

    /** Allocates everything it needs (D.1.5); never called from the audio thread. */
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;

    /** Clears every measurement. Called from `reset()`, on transport stop and on a section
        change, because the integrated reading is "since this section started". */
    virtual void reset() = 0;

    /** Audio thread, once per block, on the **input** (the mix before any gain of ours). */
    virtual void process (const juce::AudioBuffer<float>& input, int numSamples) noexcept = 0;

    /** Audio thread and message thread: the latest unsmoothed readings. */
    virtual MixReading read() const noexcept = 0;
};

/** The meter that measures nothing and says so (`setMixAnalyser (nullptr)`). With it the
    match gain is 0.0 dB in every mode - the plugin switches, loops, follows the transport
    and stays a wire on render, but it does not level-match. */
class NoMixAnalyser final : public MixAnalyser
{
public:
    void prepare (double, int) override {}
    void reset() override {}
    void process (const juce::AudioBuffer<float>&, int) noexcept override {}
    MixReading read() const noexcept override { return {}; }
};
} // namespace absee
