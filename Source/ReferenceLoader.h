#pragma once

#include "ReferenceSections.h"
#include "ReferenceStore.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

/*  A B see - the plugin's half of "References publishes, the store hands out".

    `ReferenceStore` has one writer, and this is it: everything that touches the store runs on
    the message thread (a 10 Hz timer). Decoding, resampling and measuring the sections happen
    on one worker thread (`oberli::references::load` + `buildReferenceBuffer`) and come back
    to the message thread as a finished buffer, which is then published. The audio thread
    only ever sees the store.

    Each tick (`update`):
      1. follow the Reference parameter: `store.setActive` (the store keeps that slot and the
         one before, the brief's "at most 2 decoded buffers");
      2. if the active slot's buffer is missing or was built from another path, other marks or
         another session rate, ask the worker for it (a newer request cancels an older one);
      3. publish what the worker finished, if it still matches what the slot wants (a refused
         publish is retried next tick: `ReferenceStore::publish` refuses rather than deletes);
      4. collect the store's garbage.

    A reloaded slot is what a restored project, a session-rate change and a marker change all
    need, so the three are one rule: the buffer's key (path, marks, rate) differs from the
    slot's. `update (true)` does the worker's job inline: the harness's `--loader=plugin`
    renders through this exact path without a message loop. */
namespace absee
{
class ReferenceLoader final : private juce::Timer, private juce::Thread
{
public:
    /** `activeSlot` and `slotState` are read on the message thread only. */
    ReferenceLoader (ReferenceStore& store, std::function<int()> activeSlot,
                     std::function<const SlotState& (int)> slotState);
    ~ReferenceLoader() override;

    /** Any thread (prepareToPlay): the rate references must be resampled to. */
    void setSessionRate (double rate) noexcept { sessionRate.store (rate, std::memory_order_release); }

    /** Message thread: one tick. `synchronous` decodes on the calling thread instead. */
    void update (bool synchronous = false);

    /** Message thread: the last load error of a slot in plain words, empty when none. */
    juce::String errorFor (int slot) const { return errors[(size_t) juce::jlimit (0, kNumSlots - 1, slot)]; }

    /** Message thread: true while the worker is decoding or a finished buffer waits. */
    bool isBusy() const;

private:
    struct Job
    {
        int slot = -1;
        juce::String key;
        SlotState state;
        double rate = 0.0;
    };

    struct Result
    {
        int slot = -1;
        juce::String key;
        std::unique_ptr<ReferenceBuffer> buffer;
        juce::String error;
    };

    static juce::String keyFor (const SlotState&, double rate);
    static Result runJob (const Job&, const std::atomic<bool>* cancel);
    void accept (Result&&);
    void timerCallback() override { update(); }
    void run() override;

    ReferenceStore& store;
    std::function<int()> activeSlot;
    std::function<const SlotState& (int)> slotState;
    std::atomic<double> sessionRate { 0.0 };

    std::array<juce::String, kNumSlots> publishedKey;   // message thread
    std::array<juce::String, kNumSlots> errors;          // message thread
    juce::String requestedKey;                           // message thread: the job in flight
    Result waiting;                                      // message thread: refused by the store, retried

    juce::CriticalSection lock;                          // worker <-> message thread, never audio
    std::unique_ptr<Job> pending;
    std::unique_ptr<Result> finished;
    std::atomic<bool> cancel { false };
    juce::WaitableEvent wake;

    JUCE_DECLARE_NON_COPYABLE (ReferenceLoader)
};
} // namespace absee
