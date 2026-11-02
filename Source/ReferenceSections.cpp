#include "ReferenceSections.h"
#include "BandDeltas.h"

namespace absee
{
namespace
{
struct Measurement
{
    double integratedLufs = -300.0;
    double truePeakDb = -400.0;
};

Measurement measure (oberli::Loudness& meter, const juce::AudioBuffer<float>& audio, int from, int n)
{
    meter.reset();
    const int channels = juce::jmin (2, audio.getNumChannels());
    const float* pointers[2] = { nullptr, nullptr };
    for (int ch = 0; ch < channels; ++ch)
        pointers[ch] = audio.getReadPointer (ch, from);
    meter.process (pointers, channels, n);
    const auto r = meter.readings();
    Measurement m;
    if (r.integrated > oberli::Loudness::kSilence)
        m.integratedLufs = r.integrated;
    if (r.truePeak > oberli::Loudness::kSilence)
        m.truePeakDb = r.truePeak;
    return m;
}

SectionAnalysis analyseWith (oberli::Loudness& meter, const juce::AudioBuffer<float>& audio, double rate,
                             double inSeconds, double outSeconds)
{
    const int total = audio.getNumSamples();
    const double length = (double) total / rate;
    SectionAnalysis s;
    s.inSeconds = juce::jlimit (0.0, length, inSeconds);
    s.outSeconds = juce::jlimit (s.inSeconds, length, outSeconds);
    const int from = (int) std::llround (s.inSeconds * rate);
    const int n = juce::jmin (total - from, (int) std::llround ((s.outSeconds - s.inSeconds) * rate));
    if (n <= 0)
        return s;
    const auto m = measure (meter, audio, from, n);
    s.integratedLufs = m.integratedLufs;
    s.truePeakDb = m.truePeakDb;
    s.bands = measureBands (audio, rate, s.inSeconds, s.outSeconds);
    return s;
}
} // namespace

SectionAnalysis analyseSection (const juce::AudioBuffer<float>& audio, double sampleRate,
                                double inSeconds, double outSeconds)
{
    oberli::Loudness meter;
    meter.prepare (sampleRate, juce::jmax (1, juce::jmin (2, audio.getNumChannels())));
    return analyseWith (meter, audio, sampleRate, inSeconds, outSeconds);
}

std::unique_ptr<ReferenceBuffer> buildReferenceBuffer (oberli::references::Track&& track, const SlotState& slot,
                                                       const std::atomic<bool>* cancel)
{
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load (std::memory_order_relaxed); };
    const double rate = track.sampleRate;
    if (rate <= 0.0 || track.audio.getNumSamples() == 0)
        return {};

    auto buffer = std::make_unique<ReferenceBuffer>();
    buffer->sampleRate = rate;
    buffer->name = track.name;
    buffer->audio = std::move (track.audio);
    const auto& audio = buffer->audio;
    const int total = audio.getNumSamples();
    const double length = (double) total / rate;

    oberli::Loudness meter;
    meter.prepare (rate, juce::jmax (1, juce::jmin (2, audio.getNumChannels())));

    buffer->sections[0] = analyseWith (meter, audio, rate, 0.0, length);    // Whole
    for (int m = 0; m < 4; ++m)                                             // the user's markers
    {
        const double in = slot.marks[(size_t) (m * 2)], out = slot.marks[(size_t) (m * 2 + 1)];
        buffer->sections[(size_t) (1 + m)] = out > in ? analyseWith (meter, audio, rate, in, out) : SectionAnalysis {};
    }

    // Loudest 10 s: the window with the highest integrated loudness, found on load
    const double window = juce::jmin (kLoudestWindowSeconds, length);
    double bestStart = 0.0, best = -400.0;
    for (double start = 0.0; start + window <= length + 1.0e-9; start += kLoudestHopSeconds)
    {
        if (cancelled())
            return {};
        const int from = (int) std::llround (start * rate);
        const int n = juce::jmin (total - from, (int) std::llround (window * rate));
        if (n <= 0)
            break;
        const double l = measure (meter, audio, from, n).integratedLufs;
        if (l > best)
        {
            best = l;
            bestStart = start;
        }
    }
    buffer->sections[5] = analyseWith (meter, audio, rate, bestStart, bestStart + window);
    return buffer;
}
} // namespace absee
