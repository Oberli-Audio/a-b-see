#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "oberli/Buttons.h"
#include "oberli/Knob.h"
#include "oberli/Meters.h"
#include "oberli/StripEditor.h"

/*  A B see - the editor: the family's Strip frame (Source/oberli/StripEditor.h) around the body
    of docs/sketch.md 2, 960 x 600 at 100 %, Bone, accent Peacock only on what is active.

        REFERENCES          | LISTEN  [ Mix | Reference ]  ( Reference +6.0 dB )   | OUTPUT
        1  Chorus master  o |         [ Match ............ ]  Blind              |  Trim
        2  Empty            |         Mono  Follow transport  Trim mix instead   |  meter
        ...                 |         (blind: Prefer X  Prefer Y  Same  Reveal)  |  Match
        Add...  Clear       | BALANCE  the seven band deltas, the hero           |  target
        SECTION [Whole 1 2 3 4 Loudest 10 s]

    The hero is what the ear cannot judge: the band deltas (BandDeltas.h, mix minus reference,
    level matched) and the match readout. Everything it shows is read from the processor's
    atomics in advanceBody (one frame clock, D.6.2); the blind tally is editor state, lost on
    close, by design (sketch 4.4). */
class ABSeeAudioProcessorEditor final : public oberli::ui::StripEditor
{
public:
    explicit ABSeeAudioProcessorEditor (ABSeeAudioProcessor&);
    ~ABSeeAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;

    bool previewDrag (const juce::String& parameterId) override;

    // Audio files dropped on the editor fill a reference slot; presets go to the frame.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** The slots as the list draws them: message thread. */
    struct SlotView
    {
        juce::String name;      // empty: the slot is empty
        juce::String problem;   // a load error in plain words, or empty
        bool loading = false;
    };

private:
    class SlotList;
    class DeltaChart;
    class MatchReadout;
    class SectionChips;

    void layoutBody (juce::Rectangle<int> area) override;
    void advanceBody (double now, float dt) override;
    void presetRecalled (double now) override;

    int activeSlot() const;
    void selectSlot (int slot);
    void chooseReference (int slot);
    void setReference (int slot, const juce::File& file);
    void vote (int choice);   // 0 prefer X, 1 prefer Y, 2 no difference
    void reveal();
    juce::String readoutText() const;

    ABSeeAudioProcessor& audioProcessor;   // not `processor`: AudioProcessorEditor has one (-Wshadow)

    std::unique_ptr<SlotList> slots;
    std::unique_ptr<DeltaChart> chart;
    std::unique_ptr<MatchReadout> readout;

    oberli::ui::SegmentedControl listen, listenBlind, match;
    std::unique_ptr<SectionChips> section;
    oberli::ui::PillToggle blind, mono, follow, trimMixInstead;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> pillAttachments;
    oberli::ui::ActionButton addReference, clearReference;
    oberli::ui::ActionButton preferX, preferY, noDifference, revealButton;
    oberli::ui::ParameterKnob trim, matchTarget;
    oberli::ui::LevelMeter meter;
    std::unique_ptr<juce::FileChooser> chooser;

    // the blind tally (sketch 4.4): editor state, never saved
    int votesMix = 0, votesReference = 0, votesSame = 0;
    bool revealed = false;
    bool lastBlind = false;

    juce::Rectangle<int> referencesTitle, sectionTitle, sectionCaption, listenTitle, balanceTitle, balanceCaption,
                         outputTitle, ruleLeft, ruleRight;
    juce::String sectionText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ABSeeAudioProcessorEditor)
};
