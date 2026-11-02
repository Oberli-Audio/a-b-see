#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#include "BandDeltas.h"
#include "BlindRound.h"
#include "Crossfade.h"
#include "MatchEngine.h"
#include "MixAnalysis.h"
#include "Params.h"
#include "ReferenceLoader.h"
#include "ReferencePlayer.h"
#include "ReferenceStore.h"
#include "RenderSafety.h"
#include "State.h"

/*  A B see - the processor.

    Signal flow (brief, sketch 4.1):

        in --+------------------------------ (wire) --------------+
             +-- mix meter (oberli::Loudness)                     |-- switch -- Trim -- Mono -- out
        reference RAM -- section loop / transport map -- match gain +

    Thread model (rule 1, D.1): the audio thread reads parameters through cached atomics,
    takes the active reference buffer's pointer once per block (D.1.4 pattern B), and does
    not allocate, lock, log or free. Every buffer is sized in `prepareToPlay`; a block larger
    than the announced size is sub-blocked, never a reallocation (D.5.1).

    What is deliberately not in this file: the BS.1770 meter (`MixAnalysis.h` says why) and
    anything that decodes, resamples or caches a reference (`ReferenceStore.h` says why). */
class ABSeeAudioProcessor final : public juce::AudioProcessor
{
public:
    ABSeeAudioProcessor();
    ~ABSeeAudioProcessor() override = default;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }   // nothing rings; the reference stops
    juce::AudioProcessorParameter* getBypassParameter() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }   // not empty: the Steinberg validator fails "Scan Programs"
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** setStateInformation's work, with a verdict: false when the bytes are not a state of
        this plugin (foreign, truncated, newer); the state is then unchanged. */
    bool loadState (const void* data, int sizeInBytes);

    /** The parameter tree; the editor attaches to it, the harness sets values through it. */
    juce::AudioProcessorValueTreeState& parameters() { return apvts; }

    //================================================================ the seams
    /** The slots as the audio thread sees them. `oberli::References` publishes into this. */
    absee::ReferenceStore& referenceStore() noexcept { return store; }

    /** Decodes and publishes the slots' references (message thread, a 10 Hz timer). */
    absee::ReferenceLoader& referenceLoader() noexcept { return loader; }

    /** The project's half of a reference (path, marks, align pair). Message thread. */
    absee::SlotState& slotState (int index) noexcept
    {
        return extra.slots[(size_t) juce::jlimit (0, absee::kNumSlots - 1, index)];
    }

    /** Replaces the processor's loudness meter (by default `absee::LoudnessMixAnalyser`, the
        shared `oberli::Loudness`); the harness injects fixed readings so the match policy can
        be measured without the meter's warm-up. nullptr means `NoMixAnalyser`. */
    void setMixAnalyser (std::unique_ptr<absee::MixAnalyser> analyser);

    absee::RenderSafety& renderSafety() noexcept { return safety; }

    /** The mix side of the band deltas; its timer drains it, the harness drains it by hand. */
    absee::MixBands& mixBands() noexcept { return mixSpectrum; }

    /** Defining feature 3: the seven band deltas, mix minus reference, level matched (BandDeltas.h).
        Any thread; every band invalid while no reference plays. */
    absee::BandDeltaReading bandDeltas() const noexcept
    {
        absee::BandLevels reference;
        for (size_t b = 0; b < (size_t) absee::kNumBands; ++b)
            reference[b] = publishedRefBands[b].load (std::memory_order_relaxed);
        return absee::bandDeltas (mixSpectrum.levels(), matchMixDb(), reference, matchReferenceDb());
    }
    absee::BandLevels mixBandLevels() const noexcept { return mixSpectrum.levels(); }
    absee::BandLevels referenceBandLevels() const noexcept
    {
        absee::BandLevels reference;
        for (size_t b = 0; b < (size_t) absee::kNumBands; ++b)
            reference[b] = publishedRefBands[b].load (std::memory_order_relaxed);
        return reference;
    }
    absee::BlindRound& blindRound() noexcept { return blind; }

    /** Seeds every random source in the plugin (rule 8): the harness's `--seed`. */
    void setRandomSeed (juce::uint32 seed) noexcept { blind.setSeed (seed); }

    //================================================================ what the editor reads
    /** The 20 ms RMS per channel of the output (D.6.2). */
    float outputLevel (int channel) const noexcept
    {
        return channel == 0 ? rmsL.load (std::memory_order_relaxed) : rmsR.load (std::memory_order_relaxed);
    }

    float matchReferenceDb() const noexcept { return publishedRefDb.load (std::memory_order_relaxed); }
    float matchMixDb() const noexcept { return publishedMixDb.load (std::memory_order_relaxed); }
    /** Defining feature 2, as the readout prints it. Message thread. */
    juce::String matchReadout() const { return match.readout(); }
    bool renderWireForced() const noexcept { return publishedWire.load (std::memory_order_relaxed); }
    bool referenceRateMismatch() const noexcept { return publishedMismatch.load (std::memory_order_relaxed); }
    bool referenceIsPlaying() const noexcept { return publishedRefPlaying.load (std::memory_order_relaxed); }
    int listenSide() const noexcept { return publishedSide.load (std::memory_order_relaxed); }
    double referencePlayheadSeconds() const noexcept { return publishedPlayhead.load (std::memory_order_relaxed); }
    double activeSectionIn() const noexcept { return publishedSectionIn.load (std::memory_order_relaxed); }
    double activeSectionOut() const noexcept { return publishedSectionOut.load (std::memory_order_relaxed); }

private:
    /** One-pole smoother in the dB domain, tau in seconds (parameters.csv:
        `smoothing`/`smoothing_domain`). The same difference equation as the prototype's
        `one_pole`, so the two null against each other at G5. At rest with a target of
        exactly 0 dB the state is exactly 0 dB, which is what keeps the wire a wire. */
    struct OnePoleDb
    {
        float coeff = 0.0f, value = 0.0f;

        void prepare (double sampleRate, double tauSeconds) noexcept
        {
            coeff = (float) std::exp (-1.0 / (juce::jmax (1.0e-6, tauSeconds) * sampleRate));
        }
        void reset (float db) noexcept { value = db; }
        /** Exact, on purpose: the wire path needs "no gain at all", not "almost none"
            (juce::exactlyEqual says so to the compiler as well as to the reader). */
        bool isAt (float db) const noexcept { return juce::exactlyEqual (value, db); }
        void advance (float* gain, int n, float targetDb) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                value = targetDb + coeff * (value - targetDb);
                gain[i] = juce::Decibels::decibelsToGain (value);
            }
        }
    };

    void renderChunk (juce::AudioBuffer<float>& chunk, bool wireOnly);
    float parameterValue (const char* id) const noexcept;

    juce::AudioProcessorValueTreeState apvts;

    // cached parameter pointers (never string lookups on the audio thread, D.1)
    std::atomic<float>* listenParam = nullptr;
    std::atomic<float>* refSlotParam = nullptr;
    std::atomic<float>* matchModeParam = nullptr;
    std::atomic<float>* blindParam = nullptr;
    std::atomic<float>* trimParam = nullptr;
    std::atomic<float>* monoParam = nullptr;
    std::atomic<float>* followTransportParam = nullptr;
    std::atomic<float>* trimMixInsteadParam = nullptr;
    std::atomic<float>* sectionParam = nullptr;
    std::atomic<float>* matchTargetParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    absee::ReferenceStore store;
    absee::ReferencePlayer player;
    absee::MatchEngine match;
    absee::Crossfade switchFade;
    absee::RenderSafety safety;
    absee::BlindRound blind;
    std::unique_ptr<absee::MixAnalyser> mixAnalyser;
    absee::MixBands mixSpectrum;
    int lastBandSlot = -1, lastBandSection = -1;   // audio thread: a change restarts the mix's average
    absee::State::Extra extra;
    // after the store and the slots it reads: destroyed first, its thread stopped before they go
    absee::ReferenceLoader loader {
        store,
        [this] { return refSlotParam != nullptr ? (int) std::lround (refSlotParam->load()) : 0; },
        [this] (int i) -> const absee::SlotState& { return extra.slots[(size_t) juce::jlimit (0, absee::kNumSlots - 1, i)]; } };
    OnePoleDb trimSmoothed;

    double currentSampleRate = 48000.0;
    int preparedBlockSize = 512;
    juce::AudioBuffer<float> referenceScratch;
    juce::HeapBlock<float> refGain, mixGain, fadeMixGain, fadeRefGain, trimGain;

    std::atomic<float> rmsL { 0.0f }, rmsR { 0.0f };
    juce::dsp::BallisticsFilter<float> rmsFilterL, rmsFilterR;

    std::atomic<float> publishedRefDb { 0.0f }, publishedMixDb { 0.0f };
    std::atomic<bool> publishedWire { false }, publishedMismatch { false }, publishedRefPlaying { false };
    std::atomic<int> publishedSide { 0 };
    std::atomic<double> publishedPlayhead { 0.0 }, publishedSectionIn { 0.0 }, publishedSectionOut { 0.0 };
    std::array<std::atomic<float>, absee::kNumBands> publishedRefBands {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABSeeAudioProcessor)
};
