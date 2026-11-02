#pragma once

#include "Bands.h"
#include "oberli/Analysis.h"

#include <array>
#include <atomic>
#include <vector>

/*  A B see - defining feature 3: seven band deltas, the mix minus the reference, level matched.

    The bands are the sketch's (4.5), fixed: Sub < 60, Low 60-150, Lo-mid 150-400, Mid
    400-1.5k, Hi-mid 1.5-4k, High 4-10k, Air > 10k (Hz). Each is the long-term energy of the
    shared `oberli::Spectrum`'s 1/6-octave bands whose centre falls inside its edges (an
    IEC 61260 band straddling an edge belongs to the side its centre is on; at 1/6 octave that
    moves an edge by at most a twelfth of an octave). Both sides are measured with the same
    Spectrum settings, each channel on its own and the powers summed, as BS.1770 sums them:
    Spectrum's own mid signal would read a wide mix 3 dB below a mono-ish reference of the
    same loudness in every band, a width difference shown as a tonal one (measured
    2026-11-02, pink noise L=R against uncorrelated: every delta +3 dB). Width is the sketch's
    separate width delta.

      reference  every section of a reference, offline, when it is loaded (ReferenceSections)
      mix        the plugin's input, copied by the audio thread into the shared lock-free
                 `AudioCopy` and analysed by one consumer: a 15 Hz message-thread timer in the
                 plugin, the harness between blocks (so a render is reproducible, rule 8).
                 The long-term average restarts when the active slot or section changes,
                 like the Integrated reading: both sides then cover "this section"

    delta[b] = (mix[b] + mix match gain) - (reference[b] + reference match gain), in dB: what
    the ear hears after the switch, so a level difference the match removed is not shown as
    a tonal one. The editor reads the deltas, the harness prints them, the test checks them:
    one object (rule 9 for analysis). */
namespace absee
{
/** The Spectrum settings both sides use. */
inline constexpr auto kBandResolution = oberli::BandLayout::Resolution::sixth;
inline constexpr int kBandFftOrder = 13;

/** The seven bands from two Spectrums' long-term 1/6-octave levels, one per channel, the
    powers summed (each Spectrum fed the same channel twice, so its "mid" is that channel). */
BandLevels sevenBands (const oberli::Spectrum& left, const oberli::Spectrum& right);

/** Long-term seven-band levels of `[inSeconds, outSeconds)` of `audio` (offline; allocates). */
BandLevels measureBands (const juce::AudioBuffer<float>& audio, double sampleRate, double inSeconds, double outSeconds);

/** The deltas, mix minus reference, in dB; a band either side has no energy in reads 0 and
    `valid` false for that band. */
struct BandDeltaReading
{
    BandLevels deltaDb {};
    std::array<bool, kNumBands> valid {};
};

BandDeltaReading bandDeltas (const BandLevels& mix, float mixGainDb, const BandLevels& reference, float referenceGainDb);

/** The mix side: audio thread in, one consumer out. */
class MixBands final : private juce::Timer
{
public:
    MixBands();
    ~MixBands() override;

    /** Allocates; never concurrent with `push`. Serialised against `drain` by a lock (both
        are off the audio thread). */
    void prepare (double sampleRate);

    /** Audio thread: copy the block and return (never blocks, never allocates). */
    void push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

    /** Any thread: start the long-term average again at the next drain. */
    void requestReset() noexcept { resetRequested.store (true, std::memory_order_release); }

    /** The consumer: analyse everything copied so far. The timer calls it; the harness too. */
    void drain();

    /** Any thread: the latest published levels. */
    BandLevels levels() const noexcept;
    int droppedFrames() const noexcept { return copy.dropped(); }

private:
    void timerCallback() override { drain(); }

    juce::CriticalSection lock;     // prepare <-> drain; never taken on the audio thread
    oberli::AudioCopy copy;
    oberli::Spectrum spectrumLeft, spectrumRight;
    std::vector<float> left, right;
    bool prepared = false;
    std::atomic<bool> resetRequested { false };
    std::array<std::atomic<float>, kNumBands> published {};
};
} // namespace absee
