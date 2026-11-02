#pragma once

#include "ReferenceStore.h"
#include "oberli/References.h"

#include <atomic>
#include <memory>

/*  A B see - from a decoded reference to what the audio thread plays: the sections.

    `oberli::references::load` decodes and resamples to the session rate; this builds the
    `ReferenceBuffer` the store hands out, with every section measured by the shared
    `oberli::Loudness` (rule 17: one meter in the product, the one `oberli_loudness` tests):

      0  Whole           the file
      1-4  the markers   the slot's in/out pairs; an empty pair is an unusable section
      5  Loudest 10 s    the 10 s window, on a 0.5 s grid, with the highest integrated
                         loudness (the prototype's search, so G5 can null against it)

    Off the audio thread only: it allocates, and the loudest-window search reads the file
    about twenty times over. `cancel` is polled between windows. */
namespace absee
{
inline constexpr double kLoudestWindowSeconds = 10.0;
inline constexpr double kLoudestHopSeconds = 0.5;

/** Moves `track`'s audio into a new buffer and measures its sections; nullptr when cancelled. */
std::unique_ptr<ReferenceBuffer> buildReferenceBuffer (oberli::references::Track&& track, const SlotState& slot,
                                                       const std::atomic<bool>* cancel = nullptr);

/** One section's integrated loudness and true peak, by the shared meter. */
SectionAnalysis analyseSection (const juce::AudioBuffer<float>& audio, double sampleRate,
                                double inSeconds, double outSeconds);
} // namespace absee
