#include "ReferenceLoader.h"

namespace absee
{
namespace
{
constexpr int kTicksPerSecond = 10;
}

ReferenceLoader::ReferenceLoader (ReferenceStore& s, std::function<int()> active,
                                  std::function<const SlotState& (int)> state)
    : juce::Thread ("A B see reference loader"), store (s), activeSlot (std::move (active)),
      slotState (std::move (state))
{
    startThread (juce::Thread::Priority::low);
    startTimerHz (kTicksPerSecond);
}

ReferenceLoader::~ReferenceLoader()
{
    stopTimer();
    cancel.store (true);
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

juce::String ReferenceLoader::keyFor (const SlotState& s, double rate)
{
    if (s.isEmpty() || rate <= 0.0)
        return {};
    juce::String key = s.path + "|" + juce::String (rate, 1);
    for (auto m : s.marks)
        key << "|" << juce::String (m, 6);
    return key;
}

ReferenceLoader::Result ReferenceLoader::runJob (const Job& job, const std::atomic<bool>* cancelFlag)
{
    Result r;
    r.slot = job.slot;
    r.key = job.key;
    auto track = oberli::references::load (juce::File (job.state.path), job.rate, r.error, cancelFlag);
    if (track != nullptr)
        r.buffer = buildReferenceBuffer (std::move (*track), job.state, cancelFlag);
    if (r.buffer == nullptr && r.error.isEmpty())
        r.error = "cancelled";
    return r;
}

void ReferenceLoader::run()
{
    while (! threadShouldExit())
    {
        wake.wait (-1);
        std::unique_ptr<Job> job;
        {
            const juce::ScopedLock sl (lock);
            job = std::move (pending);
        }
        if (job == nullptr)
            continue;
        cancel.store (false);
        auto result = std::make_unique<Result> (runJob (*job, &cancel));
        const juce::ScopedLock sl (lock);
        if (pending == nullptr)              // a newer request supersedes this result
            finished = std::move (result);
    }
}

bool ReferenceLoader::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return requestedKey.isNotEmpty() || waiting.buffer != nullptr || finished != nullptr;
}

void ReferenceLoader::accept (Result&& r)
{
    if (! juce::isPositiveAndBelow (r.slot, kNumSlots))
        return;
    if (r.key != requestedKey)
        return;                              // stale: the slot wants something else now
    requestedKey = {};
    if (r.buffer == nullptr)
    {
        errors[(size_t) r.slot] = r.error;
        publishedKey[(size_t) r.slot] = r.key;   // do not retry a file that cannot be read every tick
        return;
    }
    errors[(size_t) r.slot] = {};
    waiting = std::move (r);
}

void ReferenceLoader::update (bool synchronous)
{
    const double rate = sessionRate.load (std::memory_order_acquire);
    const int active = juce::jlimit (0, kNumSlots - 1, activeSlot());
    store.setActive (active);

    // 1. what the worker finished
    {
        std::unique_ptr<Result> done;
        {
            const juce::ScopedLock sl (lock);
            done = std::move (finished);
        }
        if (done != nullptr)
            accept (std::move (*done));
    }

    // 2. publish (or retry publishing) a finished buffer that still matches its slot
    if (waiting.buffer != nullptr)
    {
        const auto& s = slotState (waiting.slot);
        if (keyFor (s, rate) != waiting.key)
            waiting = {};
        else if (store.publish (waiting.slot, waiting.buffer))
        {
            publishedKey[(size_t) waiting.slot] = waiting.key;
            waiting = {};
        }
    }

    // 3. does the active slot need a (re)load?
    const auto& state = slotState (active);
    const auto want = keyFor (state, rate);
    const bool resident = store.acquire (active) != nullptr;
    if (want.isEmpty())
    {
        if (resident)
        {
            std::unique_ptr<ReferenceBuffer> none;
            if (store.publish (active, none))      // the slot was cleared: retire its buffer
                publishedKey[(size_t) active] = {};
        }
    }
    else if ((! resident || publishedKey[(size_t) active] != want) && requestedKey != want
             && waiting.key != want && ! (publishedKey[(size_t) active] == want && errors[(size_t) active].isNotEmpty()))
    {
        Job job { active, want, state, rate };
        requestedKey = want;
        if (synchronous)
        {
            accept (runJob (job, nullptr));
            if (waiting.buffer != nullptr && store.publish (waiting.slot, waiting.buffer))
            {
                publishedKey[(size_t) waiting.slot] = waiting.key;
                waiting = {};
            }
        }
        else
        {
            cancel.store (true);                 // a load still running is for something else
            {
                const juce::ScopedLock sl (lock);
                pending = std::make_unique<Job> (std::move (job));
            }
            wake.signal();
        }
    }

    store.collectGarbage();
}
} // namespace absee
