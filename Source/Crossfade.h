#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

/*  A B see - the switch (sound-design-bob rule 2: every switch is crossfaded).

    Ported by hand from `sound design/prototype/absee_proto.py` (class Crossfade); the
    constants and the reason for each are in docs/status.md (G1 decisions). */
namespace absee
{
/** The fade law. Measured at G3 (`proto_measure.py --case switch-click`):

    - `equalGain` (a + b == 1) keeps the level when the two sides are **correlated** and can
      never overshoot. An equal-power fade peaks +3.0086 dB above a 1 kHz sine that is
      present on both sides, which fails the brief's own switch test and would clip a switch
      between two masters at -1 dBTP.
    - `equalPower` (a^2 + b^2 == 1) keeps the level when the two sides are **uncorrelated**.
      The section loop's seam joins two uncorrelated points of the same file, so it uses it.

    Both laws give exactly (1, 0) at rest on side A, which is what lets Mix mode be a wire.
*/
enum class FadeLaw { equalGain, equalPower };

inline void fadeGains (float position, FadeLaw law, float& a, float& b) noexcept
{
    if (law == FadeLaw::equalPower)
    {
        a = std::cos (position * juce::MathConstants<float>::halfPi);
        b = std::sin (position * juce::MathConstants<float>::halfPi);
    }
    else
    {
        a = 1.0f - position;
        b = position;
    }
}

/** A crossfade between side A and side B, moved at a fixed rate towards a target.

    The target is set once per block (the switch starts at the next block boundary, brief
    4.1); `advance` fills two gain ramps. The fade length is in **seconds** and converted
    against the rate in `prepare` (rule 12). */
class Crossfade
{
public:
    void prepare (double sampleRate, double fadeSeconds, FadeLaw fadeLaw) noexcept
    {
        law = fadeLaw;
        step = (float) (1.0 / juce::jmax (1.0, fadeSeconds * sampleRate));
        reset (target);
    }

    /** Puts the fade at `side` with no movement: 0 = side A, 1 = side B. */
    void reset (float side) noexcept { position = target = juce::jlimit (0.0f, 1.0f, side); }

    void setTarget (float side) noexcept { target = juce::jlimit (0.0f, 1.0f, side); }

    bool isSettled() const noexcept { return juce::exactlyEqual (position, target); }
    float currentPosition() const noexcept { return position; }
    float currentTarget() const noexcept { return target; }

    /** n samples of gain for side A and side B. */
    void advance (float* gainA, float* gainB, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (position < target)       position = juce::jmin (target, position + step);
            else if (position > target)  position = juce::jmax (target, position - step);
            fadeGains (position, law, gainA[i], gainB[i]);
        }
    }

private:
    FadeLaw law = FadeLaw::equalGain;
    float step = 1.0f, position = 0.0f, target = 0.0f;
};
} // namespace absee
