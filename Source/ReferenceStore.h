#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Bands.h"

#include <array>
#include <atomic>
#include <memory>

/*  A B see - the reference slots as the audio thread sees them.

    **This file does not decode, resample or cache anything.** Finding the file, decoding it
    through `AudioFormatManager`, resampling it to the session rate, keeping the shared
    `Oberli Audio/References/` cache keyed by path, size and mtime, and computing each
    section's loudness are the shared `oberli::References` module's job (Oberli-Audio/bob-skills,
    arriving as `Source/oberli/`). What belongs to A B see is the other half: how a 115 MB
    buffer reaches the audio thread without the audio thread ever allocating, freeing or
    waiting (dsp-selection S.5, D.1.4 pattern B), and how many of them may be resident
    (brief: at most 2 decoded buffers for 8 slots).

    The division in one sentence: **References publishes, the store hands out, the player
    reads, and nobody on the audio thread owns anything.** */
namespace absee
{
inline constexpr int kNumSlots = 8;        ///< brief: up to 8 references
inline constexpr int kNumSections = 6;     ///< Whole, 1, 2, 3, 4, Loudest 10 s (parameters.csv)
inline constexpr int kMaxResident = 2;     ///< brief: the active one and the last one
inline constexpr int kRetiredCapacity = 16;

/** One section of one reference, with the analysis `oberli::References` computed for it on
    load, off the audio thread. A marker change changes the section's loudness, so it goes
    back through References and arrives as a **new** `ReferenceBuffer`: there is exactly one
    place the audio thread learns a section's bounds and its loudness, and they can never
    disagree. */
struct SectionAnalysis
{
    double inSeconds = 0.0;
    double outSeconds = 0.0;
    double integratedLufs = -300.0;
    double truePeakDb = -400.0;
    BandLevels bands = noBandLevels();   ///< long-term, seven bands (BandDeltas.h), for feature 3

    bool isUsable() const noexcept { return outSeconds > inSeconds; }
};

/** A decoded reference, at the session rate. Built on a worker thread, handed to the store,
    read by the audio thread, deleted by the store on a worker thread. */
struct ReferenceBuffer
{
    juce::AudioBuffer<float> audio;       ///< 1 or 2 channels, at `sampleRate`
    double sampleRate = 0.0;              ///< the rate it was resampled to on load
    juce::String name;                    ///< what the editor shows (hidden in blind mode)
    std::array<SectionAnalysis, kNumSections> sections {};

    const SectionAnalysis& section (int index) const noexcept
    {
        return sections[(size_t) juce::jlimit (0, kNumSections - 1, index)];
    }
};

/** The marks and the alignment of one slot: the part of a reference that belongs in the
    **project**, not in the decoded buffer. `oberli::References` owns the file and the cache;
    this is what `getStateInformation` writes so reopening a set restores the same sections.
    Message thread only. */
struct SlotState
{
    juce::String path;                         ///< as the user chose it; References resolves it
    double cachedLufs = -300.0;                ///< whole-file integrated, from the shared cache
    double cachedTruePeakDb = -400.0;
    std::array<double, 8> marks { { 0, 0, 0, 0, 0, 0, 0, 0 } };   ///< sections 1-4 as in/out pairs, seconds
    double alignDawSeconds = 0.0;              ///< the "Align here" pair (sketch 4.3)
    double alignRefSeconds = 0.0;

    bool isEmpty() const noexcept { return path.isEmpty(); }
};

/** Two values that must change together, published to the audio thread without a lock
    (D.1.4: "a small group of values that must change together" - the sequence-lock form).

    The writer bumps the counter to odd, writes, bumps it to even. The reader reads the
    counter, the values and the counter again; if the two counters differ, or the first was
    odd, the write was in flight and the reader **keeps the pair it had** - which for an
    alignment that the user is dragging is exactly right, one block late. */
template <typename T>
class SeqLockPair
{
public:
    void write (T first, T second) noexcept
    {
        const auto gen = counter.load (std::memory_order_relaxed);
        counter.store (gen + 1, std::memory_order_release);          // odd: in flight
        a.store (first, std::memory_order_relaxed);
        b.store (second, std::memory_order_relaxed);
        counter.store (gen + 2, std::memory_order_release);          // even: settled
    }

    /** Reads into `first`/`second`; returns false when a write was in flight (the caller's
        previous values stand). */
    bool read (T& first, T& second) const noexcept
    {
        const auto g1 = counter.load (std::memory_order_acquire);
        if ((g1 & 1u) != 0u)
            return false;
        const T x = a.load (std::memory_order_relaxed);
        const T y = b.load (std::memory_order_relaxed);
        if (counter.load (std::memory_order_acquire) != g1)
            return false;
        first = x;
        second = y;
        return true;
    }

private:
    std::atomic<unsigned> counter { 0 };
    std::atomic<T> a { T() }, b { T() };
};

/** The eight slots. `publish`, `retire` and `collectGarbage` are the writer's (the message
    or a worker thread, i.e. `oberli::References`); `acquire` and `blockFinished` are the
    audio thread's. The audio thread never deletes and never allocates.

    Reclamation: the audio thread counts blocks. A retired buffer may be deleted once the
    counter has advanced **twice** since it was retired, because the audio thread can have
    loaded the old pointer at the start of the block that is running now (D.1.4). */
class ReferenceStore
{
public:
    ReferenceStore() = default;

    ~ReferenceStore()
    {
        for (auto& slot : live)
            delete slot.exchange (nullptr);
        for (auto& r : retired)
            r.buffer.reset();
    }

    //--------------------------------------------------------------- writer side
    /** Hands a decoded reference to slot `index` (0-based). The previous one is retired, not
        deleted. On success `next` is empty; on a refusal (nothing could be retired safely
        yet) `next` still holds the buffer and References retries after the next block. */
    bool publish (int index, std::unique_ptr<ReferenceBuffer>& next)
    {
        if (! juce::isPositiveAndBelow (index, kNumSlots))
            return false;
        collectGarbage();
        // Room for the one we are about to retire is checked *before* the swap, so a refusal
        // costs the caller nothing: `next` is still theirs and the slot is untouched.
        if (live[(size_t) index].load (std::memory_order_acquire) != nullptr && ! hasRetiredRoom())
            return false;
        if (auto* old = live[(size_t) index].exchange (next.release(), std::memory_order_acq_rel))
            pushRetired (old);
        return true;
    }

    /** Which slot is playing. The store keeps that one and the one before it decoded and
        retires the rest, which is the brief's "at most 2 decoded buffers resident". */
    void setActive (int index)
    {
        if (index != activeSlot)
        {
            previousSlot = activeSlot;
            activeSlot = index;
        }
        for (int i = 0; i < kNumSlots; ++i)
            if (i != activeSlot && i != previousSlot
                && live[(size_t) i].load (std::memory_order_acquire) != nullptr && hasRetiredRoom())
                if (auto* old = live[(size_t) i].exchange (nullptr, std::memory_order_acq_rel))
                    pushRetired (old);
        collectGarbage();
    }

    /** Deletes every retired buffer the audio thread can no longer be holding. */
    void collectGarbage()
    {
        const auto now = blockCounter.load (std::memory_order_acquire);
        for (auto& r : retired)
            if (r.buffer != nullptr && now >= r.retiredAt + 2)
                r.buffer.reset();
    }

    /** After `releaseResources`: the audio thread is gone, everything can go. */
    void releaseAll()
    {
        for (auto& slot : live)
            delete slot.exchange (nullptr, std::memory_order_acq_rel);
        for (auto& r : retired)
            r.buffer.reset();
    }

    //--------------------------------------------------------------- audio side
    /** The buffer of slot `index`, taken **once** per block and used only through that copy. */
    const ReferenceBuffer* acquire (int index) const noexcept
    {
        if (! juce::isPositiveAndBelow (index, kNumSlots))
            return nullptr;
        return live[(size_t) index].load (std::memory_order_acquire);
    }

    /** Called at the end of every `processBlock`; drives the reclamation above. */
    void blockFinished() noexcept { blockCounter.fetch_add (1, std::memory_order_release); }

    //--------------------------------------------------------------- both
    int residentCount() const noexcept
    {
        int n = 0;
        for (auto& slot : live)
            if (slot.load (std::memory_order_acquire) != nullptr)
                ++n;
        return n;
    }

    int retiredCount() const noexcept
    {
        int n = 0;
        for (auto& r : retired)
            if (r.buffer != nullptr)
                ++n;
        return n;
    }

    int active() const noexcept { return activeSlot; }

private:
    struct Retired
    {
        std::unique_ptr<ReferenceBuffer> buffer;
        juce::uint64 retiredAt = 0;
    };

    bool hasRetiredRoom() const noexcept
    {
        for (auto& r : retired)
            if (r.buffer == nullptr)
                return true;
        return false;
    }

    bool pushRetired (ReferenceBuffer* b)
    {
        const auto now = blockCounter.load (std::memory_order_acquire);
        for (auto& r : retired)
            if (r.buffer == nullptr)
            {
                r.buffer.reset (b);
                r.retiredAt = now;
                return true;
            }
        return false;
    }

    std::array<std::atomic<ReferenceBuffer*>, kNumSlots> live {};
    std::array<Retired, kRetiredCapacity> retired {};
    std::atomic<juce::uint64> blockCounter { 0 };
    int activeSlot = 0, previousSlot = -1;

    JUCE_DECLARE_NON_COPYABLE (ReferenceStore)
};
} // namespace absee
