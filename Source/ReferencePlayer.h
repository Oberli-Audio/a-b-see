#pragma once

#include "Crossfade.h"
#include "ReferenceStore.h"

#include <juce_audio_basics/juce_audio_basics.h>

/*  A B see - reading the reference: the section loop and the transport map.

    Hand port of `sound design/prototype/absee_proto.py` (class ReferencePlayer). Audio
    thread only, no allocation: the output buffer is the processor's scratch, sized in
    `prepareToPlay`. */
namespace absee
{
/** Plays the active reference's section, looped.

    **Rate.** The buffer arrives at the session rate (`oberli::References` resamples on
    load, sketch 4.3), so reading it is an integer step and `processBlock` never resamples.
    A buffer at another rate - the moment after a sample-rate change, before References has
    re-resampled - is **not** read: the reference is silent and `rateMismatch()` says why.
    Interpolating it instead would mean a wrong-pitch reference that sounds plausible, which
    is worse than silence in a tool whose whole job is to be trusted.

    **The seam.** A loop jumps from `out` back to `in`, which is a step on any material. The
    last `seamSeconds` of the section are faded into the section's start, equal-power because
    the two points are uncorrelated (G3: without the fade a looped 100 Hz sine reads -0.0 dB
    `max_click`, with it -34.1 dB).

    **The map.** Free-running counts samples and wraps inside the section, so the reference
    plays while the DAW stands still. Following the transport takes the position from the
    host clock in **seconds** through the align pair (G1 departure 2): one DAW second is one
    reference second, so a tempo change cannot slide the reference against the music. With
    the transport stopped the reference stops with it.
*/
class ReferencePlayer
{
public:
    void prepare (double rate, double seamSeconds) noexcept
    {
        sampleRate = rate;
        seam = seamSeconds;
        reset();
    }

    void reset() noexcept
    {
        freePosition = 0;
        mismatch = false;
    }

    /** The "Align here" pair: the DAW position and the reference position that meet. */
    void setAlign (double dawSeconds, double refSeconds) noexcept
    {
        alignDaw = dawSeconds;
        alignRef = refSeconds;
    }

    bool rateMismatch() const noexcept { return mismatch; }

    /** Where in the reference the last block read from, in seconds (for the editor). */
    double playheadSeconds() const noexcept { return lastReadSeconds; }

    /** Fills `out` (2 channels, at least `n` samples) with the section.
        Returns false when there is nothing to play; `out` is cleared in that case. */
    bool render (juce::AudioBuffer<float>& out, int n, const ReferenceBuffer* buffer,
                 const SectionAnalysis& section, bool followTransport,
                 bool transportValid, bool transportPlaying, double transportSeconds) noexcept
    {
        out.clear (0, n);
        mismatch = false;
        if (buffer == nullptr || buffer->audio.getNumSamples() <= 0 || ! section.isUsable())
            return false;
        if (! juce::exactlyEqual (buffer->sampleRate, sampleRate))
        {
            mismatch = true;
            return false;
        }

        const int total = buffer->audio.getNumSamples();
        const int inSample = juce::jlimit (0, juce::jmax (0, total - 1),
                                           (int) std::llround (section.inSeconds * sampleRate));
        const int outSample = juce::jlimit (inSample + 1, total,
                                            (int) std::llround (section.outSeconds * sampleRate));
        const int length = outSample - inSample;
        if (length <= 1)
            return false;

        juce::int64 start = 0;
        if (followTransport)
        {
            if (! transportValid || ! transportPlaying)
                return false;                                  // the DAW stands still: so do we
            const double offset = (alignRef - section.inSeconds) + (transportSeconds - alignDaw);
            double wrapped = std::fmod (offset, (double) length / sampleRate);
            if (wrapped < 0.0)
                wrapped += (double) length / sampleRate;
            start = (juce::int64) std::llround (wrapped * sampleRate) % length;
        }
        else
        {
            start = freePosition % length;
            freePosition = (freePosition + n) % length;
        }

        const int seamSamples = juce::jlimit (1, juce::jmax (1, length / 2),
                                              (int) std::llround (seam * sampleRate));
        const int channels = juce::jmin (2, out.getNumChannels());
        const int srcChannels = buffer->audio.getNumChannels();

        for (int i = 0; i < n; ++i)
        {
            const int idx = (int) ((start + i) % length);
            const int from = inSample + idx;
            float gTail = 1.0f, gHead = 0.0f;
            const bool inSeam = idx >= length - seamSamples;
            if (inSeam)
            {
                const float f = (float) (idx - (length - seamSamples)) / (float) seamSamples;
                fadeGains (f, FadeLaw::equalPower, gTail, gHead);
            }
            const int head = inSample + (idx - (length - seamSamples));
            for (int ch = 0; ch < channels; ++ch)
            {
                const int src = juce::jmin (ch, srcChannels - 1);
                float v = buffer->audio.getSample (src, from) * gTail;
                if (inSeam)
                    v += buffer->audio.getSample (src, juce::jlimit (0, total - 1, head)) * gHead;
                out.setSample (ch, i, v);
            }
        }
        lastReadSeconds = (double) (inSample + (int) ((start + n - 1) % length)) / sampleRate;
        return true;
    }

private:
    double sampleRate = 48000.0, seam = 0.005;
    double alignDaw = 0.0, alignRef = 0.0, lastReadSeconds = 0.0;
    juce::int64 freePosition = 0;
    bool mismatch = false;
};
} // namespace absee
