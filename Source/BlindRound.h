#pragma once

#include <juce_core/juce_core.h>

#include <atomic>

/*  A B see - blind X/Y (brief: "Blind: hides which side is which, keeps a tally").

    The tally and the vote buttons are the editor's; what belongs here is the only part the
    audio path needs: **which of X and Y is the mix this round**. With Blind on, the Listen
    parameter still has two positions, but position 0 is X, and X is the mix or the reference
    depending on the round's draw.

    Rule 8: the draw comes from an RNG the harness can seed, so two seeded runs from
    `reset()` are bit-identical and a test can know which side it is listening to. */
namespace absee
{
class BlindRound
{
public:
    /** The harness's `--seed` and the processor's `reset()` both come through here. */
    void setSeed (juce::uint32 seed) noexcept
    {
        roundSeed = seed;
        state = (juce::uint64) seed;
        roundNumber = 0;
        drawRound();
    }

    /** A new round: X becomes the mix or the reference by a fresh draw. */
    void newRound() noexcept { drawRound(); }

    /** Back to the first draw of this seed, so a render is reproducible. */
    void reset() noexcept { setSeed (roundSeed); }

    bool xIsMix() const noexcept { return xIsMixFlag.load (std::memory_order_relaxed); }
    int round() const noexcept { return roundNumber; }

    /** Audio thread: which side to play for a Listen position, with Blind on or off.
        0 = mix, 1 = reference. */
    int sideFor (int listenPosition, bool blind) const noexcept
    {
        if (! blind)
            return listenPosition != 0 ? 1 : 0;
        const bool pickedX = listenPosition == 0;
        const bool wantMix = pickedX == xIsMix();
        return wantMix ? 0 : 1;
    }

    /** What the editor prints instead of Mix / Reference. */
    static juce::String labelFor (int listenPosition, bool blind)
    {
        if (! blind)
            return listenPosition != 0 ? "Reference" : "Mix";
        return listenPosition != 0 ? "Y" : "X";
    }

private:
    /** splitmix64, not `juce::Random`: a linear congruential generator's first output bit is
        correlated with the low bits of its seed, and `juce::Random(seed).nextBool()` returned
        **false for every seed from 1 to 12** - the round was not blind at all, it was always
        the same way round (found by `test_blind_assignment_is_seeded`, docs/failures.md
        2026-11-01). splitmix64 avalanches the seed by construction, so one draw per round is
        enough and the sequence is still exactly reproducible (rule 8). */
    static juce::uint64 splitmix64 (juce::uint64& x) noexcept
    {
        x += 0x9E3779B97F4A7C15ull;
        juce::uint64 z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    void drawRound() noexcept
    {
        xIsMixFlag.store ((splitmix64 (state) >> 63) != 0, std::memory_order_relaxed);
        ++roundNumber;
    }

    juce::uint64 state = 1;
    juce::uint32 roundSeed = 1;
    int roundNumber = 0;
    std::atomic<bool> xIsMixFlag { true };
};
} // namespace absee
