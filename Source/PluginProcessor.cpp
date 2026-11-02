#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LoudnessMixAnalyser.h"

#include <cmath>

namespace
{
std::atomic<float>* rawParam (juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* p = apvts.getRawParameterValue (id);
    jassert (p != nullptr);   // the table and the code disagree: parameters.csv is the source
    return p;
}

float tauMsFor (const char* id) noexcept
{
    for (const auto& s : Params::smoothing)
        if (std::strcmp (s.id, id) == 0)
            return s.tauMs;
    return 0.0f;
}

/** The fade length of the switch and of the loop seam, in seconds (rule 12: never samples).
    5 ms is the brief's; G1 departure 1 records it against rule 2's 10-20 ms and names the
    measurement that would reject it (`proto_measure.py --case switch-click`). */
constexpr double kSwitchFadeSeconds = 0.005;
constexpr double kSeamFadeSeconds = 0.005;
/** The mono sum, -3 dB on both channels (sketch 4.1). */
constexpr float kMonoSumDb = -3.0f;
/** The match gain's smoother (brief 4.1: "a one-pole smoother with 20 ms time constant so a
    changing short-term match does not pump"). A constant, not a row of the parameter table:
    the table's `smoothing` column is about smoothing a *parameter*, and the match gain is a
    measurement's consequence, not a parameter. */
constexpr double kMatchTauSeconds = 0.020;
} // namespace

ABSeeAudioProcessor::ABSeeAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", Params::createParameterLayout())
{
    listenParam = rawParam (apvts, Params::listen);
    refSlotParam = rawParam (apvts, Params::refSlot);
    matchModeParam = rawParam (apvts, Params::matchMode);
    blindParam = rawParam (apvts, Params::blind);
    trimParam = rawParam (apvts, Params::trim);
    monoParam = rawParam (apvts, Params::mono);
    followTransportParam = rawParam (apvts, Params::followTransport);
    trimMixInsteadParam = rawParam (apvts, Params::trimMixInstead);
    sectionParam = rawParam (apvts, Params::section);
    matchTargetParam = rawParam (apvts, Params::matchTarget);
    bypassParam = rawParam (apvts, Params::bypassParameterId);

    // The mix meter: the shared oberli::Loudness behind the MixAnalysis.h seam (rule 17)
    mixAnalyser = std::make_unique<absee::LoudnessMixAnalyser>();
    blind.setSeed (1);
    setLatencySamples (0);   // brief: 0 samples in every mode (rule 7, asserted in a test)
}

void ABSeeAudioProcessor::setMixAnalyser (std::unique_ptr<absee::MixAnalyser> analyser)
{
    if (analyser == nullptr)
        analyser = std::make_unique<absee::NoMixAnalyser>();
    analyser->prepare (currentSampleRate, preparedBlockSize);
    mixAnalyser = std::move (analyser);
}

void ABSeeAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

    // Everything that allocates happens here (D.1.5); a block larger than this is sub-blocked
    // in processBlock, never reallocated.
    referenceScratch.setSize (2, preparedBlockSize, false, true, true);
    refGain.allocate ((size_t) preparedBlockSize, true);
    mixGain.allocate ((size_t) preparedBlockSize, true);
    fadeMixGain.allocate ((size_t) preparedBlockSize, true);
    fadeRefGain.allocate ((size_t) preparedBlockSize, true);
    trimGain.allocate ((size_t) preparedBlockSize, true);

    switchFade.prepare (sampleRate, kSwitchFadeSeconds, absee::FadeLaw::equalGain);
    switchFade.reset (listenParam->load() >= 0.5f ? 1.0f : 0.0f);
    player.prepare (sampleRate, kSeamFadeSeconds);
    match.prepare (sampleRate, kMatchTauSeconds);
    trimSmoothed.prepare (sampleRate, (double) tauMsFor (Params::trim) / 1000.0);
    trimSmoothed.reset (trimParam->load());
    mixAnalyser->prepare (sampleRate, preparedBlockSize);
    loader.setSessionRate (sampleRate);   // a new rate reloads the references at it
    mixSpectrum.prepare (sampleRate);
    for (auto& b : publishedRefBands)
        b.store (absee::kNoLevel);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) preparedBlockSize, 1 };
    for (auto* f : { &rmsFilterL, &rmsFilterR })
    {
        f->prepare (spec);
        f->setLevelCalculationType (juce::dsp::BallisticsFilterLevelCalculationType::RMS);
        f->setAttackTime (20.0f);
        f->setReleaseTime (20.0f);
    }
    rmsL.store (0.0f);
    rmsR.store (0.0f);
}

void ABSeeAudioProcessor::releaseResources()
{
    store.releaseAll();   // the audio thread is gone: every decoded reference can go
}

void ABSeeAudioProcessor::reset()
{
    // rule 8: two seeded runs from here are bit-identical
    switchFade.reset (listenParam->load() >= 0.5f ? 1.0f : 0.0f);
    player.reset();
    match.reset();
    trimSmoothed.reset (trimParam->load());
    mixAnalyser->reset();
    mixSpectrum.requestReset();
    blind.reset();
    rmsFilterL.reset();
    rmsFilterR.reset();
    rmsL.store (0.0f);
    rmsR.store (0.0f);
}

bool ABSeeAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        || layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono();
}

juce::AudioProcessorParameter* ABSeeAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter (Params::bypassParameterId);
}

void ABSeeAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Hard bypass is the wire (sketch 4.6); latency is 0, so nothing to compensate (rule 7).
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    store.blockFinished();
}

void ABSeeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midi);

    const int numSamples = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
    if (numSamples == 0)   // a zero-length block is legal (harness contract, D.5.1)
        return;

    if (bypassParam->load() >= 0.5f)
    {
        processBlockBypassed (buffer, midi);
        return;
    }

    // Structural decision 3 of the sketch: an offline render is the mix, whatever the state.
    const bool wireOnly = safety.wireOnly (isNonRealtime());
    publishedWire.store (wireOnly, std::memory_order_relaxed);

    // A block larger than the announced size is split, not reallocated (D.5.1).
    for (int start = 0; start < numSamples; start += preparedBlockSize)
    {
        const int n = juce::jmin (preparedBlockSize, numSamples - start);
        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(),
                                        juce::jmin (2, buffer.getNumChannels()), start, n);
        renderChunk (chunk, wireOnly);
    }
    store.blockFinished();
}

void ABSeeAudioProcessor::renderChunk (juce::AudioBuffer<float>& chunk, bool wireOnly)
{
    const int n = chunk.getNumSamples();
    const int channels = chunk.getNumChannels();

    if (wireOnly)
    {
        // The wire: no gain, no crossfade, no reference, no metering. The switch is snapped
        // to Mix so that the first realtime block after an offline render starts from the
        // mix and fades to whatever the user had chosen, instead of jumping into it.
        switchFade.reset (0.0f);
        match.reset();
        trimSmoothed.reset (0.0f);
        publishedRefPlaying.store (false, std::memory_order_relaxed);
        publishedSide.store (0, std::memory_order_relaxed);
        publishedRefDb.store (0.0f, std::memory_order_relaxed);
        publishedMixDb.store (0.0f, std::memory_order_relaxed);
        return;
    }

    // --- the mix meter sees the input, before any gain of ours (A.0: the measurement path)
    mixAnalyser->process (chunk, n);
    const auto reading = mixAnalyser->read();

    // --- the active reference, its pointer taken once (D.1.4 pattern B)
    // refSlot is a choice ("1".."8"), so its raw value is already the 0-based index
    const int slotIndex = juce::jlimit (0, absee::kNumSlots - 1, (int) std::lround (refSlotParam->load()));
    const auto* referenceBuffer = store.acquire (slotIndex);
    const int sectionIndex = (int) std::lround (sectionParam->load());
    const absee::SectionAnalysis section = referenceBuffer != nullptr
                                             ? referenceBuffer->section (sectionIndex)
                                             : absee::SectionAnalysis {};

    // --- the mix's band levels (feature 3): copy the input and return; analysed off this thread.
    // A new slot or section starts the long-term average again, so both sides cover one section.
    if (slotIndex != lastBandSlot || sectionIndex != lastBandSection)
    {
        mixSpectrum.requestReset();
        lastBandSlot = slotIndex;
        lastBandSection = sectionIndex;
    }
    mixSpectrum.push (chunk, n);

    // --- the match policy
    const auto mode = (absee::MatchMode) juce::jlimit (0, 3, (int) std::lround (matchModeParam->load()));
    match.setTargets (mode, reading, section, trimMixInsteadParam->load() >= 0.5f, matchTargetParam->load());
    match.advance (refGain.get(), mixGain.get(), n);

    // --- the reference audio
    bool transportValid = false, transportPlaying = false;
    double transportSeconds = 0.0;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
        {
            transportValid = true;
            transportPlaying = position->getIsPlaying();
            if (const auto seconds = position->getTimeInSeconds())
                transportSeconds = *seconds;
        }
    const auto& slot = extra.slots[(size_t) slotIndex];
    player.setAlign (slot.alignDawSeconds, slot.alignRefSeconds);
    const bool haveReference = player.render (referenceScratch, n, referenceBuffer, section,
                                              followTransportParam->load() >= 0.5f,
                                              transportValid, transportPlaying, transportSeconds);

    // --- which side the switch is heading for; with no reference the switch stays on the mix
    const int listenPosition = (int) std::lround (listenParam->load());
    const bool isBlind = blindParam->load() >= 0.5f;
    const int side = haveReference ? blind.sideFor (listenPosition, isBlind) : 0;
    switchFade.setTarget ((float) side);
    switchFade.advance (fadeMixGain.get(), fadeRefGain.get(), n);

    const float trimDb = trimParam->load();
    const bool mono = monoParam->load() >= 0.5f;

    // --- the wire, exactly: Mix mode, nothing smoothing, Trim at 0 dB, Mono off. The samples
    // are then not touched at all, which is what makes the brief's `thd_n: wire` target a
    // bit-exact null rather than a number near zero.
    const bool wirePath = side == 0 && switchFade.isSettled()
                       && juce::exactlyEqual (switchFade.currentPosition(), 0.0f)
                       && juce::exactlyEqual (trimDb, 0.0f) && trimSmoothed.isAt (0.0f) && ! mono
                       && match.mixIsUntouched();
    if (! wirePath)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* out = chunk.getWritePointer (ch);
            const auto* ref = referenceScratch.getReadPointer (juce::jmin (ch, referenceScratch.getNumChannels() - 1));
            for (int i = 0; i < n; ++i)
                out[i] = out[i] * fadeMixGain[i] * mixGain[i] + ref[i] * fadeRefGain[i] * refGain[i];
        }

        // Trim: the one gain both sides share, after the switch (sketch 4.1)
        trimSmoothed.advance (trimGain.get(), n, trimDb);
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* out = chunk.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                out[i] *= trimGain[i];
        }

        if (mono && channels >= 2)
        {
            const float g = juce::Decibels::decibelsToGain (kMonoSumDb);
            auto* l = chunk.getWritePointer (0);
            auto* r = chunk.getWritePointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float m = (l[i] + r[i]) * g;
                l[i] = m;
                r[i] = m;
            }
        }
    }

    // --- what the editor reads (atomics only, D.6.2)
    float lastL = 0.0f, lastR = 0.0f;
    {
        const auto* l = chunk.getReadPointer (0);
        const auto* r = chunk.getReadPointer (channels > 1 ? 1 : 0);
        for (int i = 0; i < n; ++i)
        {
            lastL = rmsFilterL.processSample (0, l[i]);
            lastR = rmsFilterR.processSample (0, r[i]);
        }
    }
    rmsL.store (lastL, std::memory_order_relaxed);
    rmsR.store (lastR, std::memory_order_relaxed);
    publishedRefDb.store (match.referenceTargetDb(), std::memory_order_relaxed);
    publishedMixDb.store (match.mixTargetDb(), std::memory_order_relaxed);
    publishedSide.store (side, std::memory_order_relaxed);
    publishedRefPlaying.store (haveReference, std::memory_order_relaxed);
    publishedMismatch.store (player.rateMismatch(), std::memory_order_relaxed);
    publishedPlayhead.store (player.playheadSeconds(), std::memory_order_relaxed);
    publishedSectionIn.store (section.inSeconds, std::memory_order_relaxed);
    publishedSectionOut.store (section.outSeconds, std::memory_order_relaxed);
    for (size_t b = 0; b < (size_t) absee::kNumBands; ++b)
        publishedRefBands[b].store (haveReference ? section.bands[b] : absee::kNoLevel, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* ABSeeAudioProcessor::createEditor()
{
    return new ABSeeAudioProcessorEditor (*this);
}

void ABSeeAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = absee::State::toXml (apvts);
    extra.safeOnRender = safety.safeOnRender();
    absee::State::writeExtra (*xml, extra);
    copyXmlToBinary (*xml, destData);
}

void ABSeeAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    loadState (data, sizeInBytes);
}

bool ABSeeAudioProcessor::loadState (const void* data, int sizeInBytes)
{
    // arbitrary bytes are refused, never crashed on (D.5.3): a foreign or newer blob leaves
    // the state as it is
    if (data == nullptr || sizeInBytes <= 0)
        return false;
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return false;
    auto* state = absee::State::stateOf (*xml);
    if (state == nullptr || ! absee::State::fromXml (*state, apvts))
        return false;
    absee::State::Extra loaded;
    if (absee::State::readExtra (*state, loaded))
    {
        extra = loaded;
        safety.setSafeOnRender (extra.safeOnRender);
    }
    return true;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ABSeeAudioProcessor();
}
