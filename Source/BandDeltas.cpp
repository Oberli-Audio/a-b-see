#include "BandDeltas.h"

#include <cmath>

namespace absee
{
namespace
{
constexpr double kCopySeconds = 2.0;      // the consumer may stall this long before frames drop
constexpr int kTimerHz = 15;
constexpr int kChunkFrames = 4096;

int bandOf (float centreHz) noexcept
{
    for (int b = 0; b < kNumBands - 1; ++b)
        if (centreHz < kBandUpperEdgesHz[(size_t) b])
            return b;
    return kNumBands - 1;
}

double powerOf (float db) noexcept { return db > kNoLevel ? std::pow (10.0, (double) db / 10.0) : 0.0; }
float dbOf (double power) noexcept { return power > 0.0 ? (float) (10.0 * std::log10 (power)) : kNoLevel; }
} // namespace

BandLevels sevenBands (const oberli::Spectrum& left, const oberli::Spectrum& right)
{
    std::array<double, kNumBands> power {};
    const auto& layout = left.layout().bands;
    jassert (layout.size() == right.layout().bands.size());
    for (int i = 0; i < (int) layout.size(); ++i)
        power[(size_t) bandOf (layout[(size_t) i].centre)] += powerOf (left.longTermDb (i)) + powerOf (right.longTermDb (i));
    BandLevels out;
    for (int b = 0; b < kNumBands; ++b)
        out[(size_t) b] = dbOf (power[(size_t) b]);
    return out;
}

BandLevels measureBands (const juce::AudioBuffer<float>& audio, double sampleRate, double inSeconds, double outSeconds)
{
    const int total = audio.getNumSamples();
    const int from = juce::jlimit (0, total, (int) std::llround (inSeconds * sampleRate));
    const int to = juce::jlimit (from, total, (int) std::llround (outSeconds * sampleRate));
    if (to <= from || audio.getNumChannels() == 0)
        return noBandLevels();
    oberli::Spectrum sl, sr;
    sl.prepare (sampleRate, kBandResolution, kBandFftOrder);
    sr.prepare (sampleRate, kBandResolution, kBandFftOrder);
    const auto* l = audio.getReadPointer (0, from);
    const auto* r = audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0, from);
    sl.push (l, l, to - from);
    sr.push (r, r, to - from);
    return sevenBands (sl, sr);
}

BandDeltaReading bandDeltas (const BandLevels& mix, float mixGainDb, const BandLevels& reference, float referenceGainDb)
{
    BandDeltaReading r;
    for (size_t b = 0; b < (size_t) kNumBands; ++b)
    {
        r.valid[b] = mix[b] > kNoLevel && reference[b] > kNoLevel;
        r.deltaDb[b] = r.valid[b] ? (mix[b] + mixGainDb) - (reference[b] + referenceGainDb) : 0.0f;
    }
    return r;
}

MixBands::MixBands()
{
    for (auto& p : published)
        p.store (kNoLevel);
    startTimerHz (kTimerHz);
}

MixBands::~MixBands()
{
    stopTimer();
}

void MixBands::prepare (double sampleRate)
{
    const juce::ScopedLock sl (lock);
    copy.prepare (sampleRate, kCopySeconds);
    spectrumLeft.prepare (sampleRate, kBandResolution, kBandFftOrder);
    spectrumRight.prepare (sampleRate, kBandResolution, kBandFftOrder);
    left.assign ((size_t) kChunkFrames, 0.0f);
    right.assign ((size_t) kChunkFrames, 0.0f);
    prepared = true;
    for (auto& p : published)
        p.store (kNoLevel);
}

void MixBands::push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (numSamples > 0)
        copy.push (buffer, 0, numSamples);
}

void MixBands::drain()
{
    const juce::ScopedLock sl (lock);
    if (! prepared)
        return;
    if (resetRequested.exchange (false, std::memory_order_acq_rel))
    {
        spectrumLeft.reset();
        spectrumRight.reset();
    }
    bool any = false;
    for (int n; (n = copy.pop (left.data(), right.data(), kChunkFrames)) > 0;)
    {
        spectrumLeft.push (left.data(), left.data(), n);
        spectrumRight.push (right.data(), right.data(), n);
        any = true;
    }
    if (! any)
        return;
    const auto levels = sevenBands (spectrumLeft, spectrumRight);
    for (size_t b = 0; b < (size_t) kNumBands; ++b)
        published[b].store (levels[b], std::memory_order_relaxed);
}

BandLevels MixBands::levels() const noexcept
{
    BandLevels out;
    for (size_t b = 0; b < (size_t) kNumBands; ++b)
        out[b] = published[b].load (std::memory_order_relaxed);
    return out;
}
} // namespace absee
