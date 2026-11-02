#pragma once

#include <juce_core/juce_core.h>

/*  A B see - render safety: the rule that makes this plugin usable on a master bus.

    Structural decision 3 of the sketch and defining feature 1 of the brief: an offline
    render is the mix, passed through, whatever state the plugin was left in - Reference
    mode, Blind on, Trim at +6 dB, Mono on. Ableton Live's export sets non-realtime, and so
    do Freeze and Bounce in Place.

    It is a policy object rather than three lines inside `processBlock` so that the one test
    that decides whether this plugin may ship at all (`test_render_is_always_the_wire`) has
    something it can state in one sentence, and so that the shared `oberli::RenderSafety`
    (bob-skills) can replace it without touching the DSP. */
namespace absee
{
class RenderSafety
{
public:
    /** The menu item "Safe on render", on by default. Deliberately **not** a host parameter:
        a parameter can be automated, and a project that automates this to off would silently
        print the reference into a bounce. */
    void setSafeOnRender (bool shouldBeSafe) noexcept { safe = shouldBeSafe; }
    bool safeOnRender() const noexcept { return safe; }

    /** True when this block must be the wire: the host is rendering offline and the safety
        is on. The caller passes `AudioProcessor::isNonRealtime()`. */
    bool wireOnly (bool hostIsNonRealtime) const noexcept { return safe && hostIsNonRealtime; }

private:
    bool safe = true;
};
} // namespace absee
