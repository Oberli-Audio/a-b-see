#include "PluginEditor.h"
#include "BuildInfo.h"
#include "Params.h"
#include "State.h"

#include <ProductData.h>

#include <cmath>

namespace
{
using namespace oberli::ui;

constexpr int kBodyWidth = 960 - 2 * Tokens::bodyPadding;                          // sketch 2: 960 x 600
constexpr int kBodyHeight = 600 - Tokens::headerHeight - 2 * Tokens::bodyPadding;
constexpr int kReferencesWidth = 232;
constexpr int kOutputWidth = 96;
constexpr int kGroupGap = Tokens::space6;
constexpr int kTitleHeight = 14;
constexpr int kRowHeight = Tokens::controlHeight;
constexpr int kListenHeight = 40;            // the switch the hand needs (sketch 2.2)
constexpr float kChartRangeDb = 6.0f;        // the bars' full height either side of the centre line
constexpr float kLookHereDb = 2.5f;
constexpr float kBarWidth = 18.0f;          // sketch 4.5: +-1 dB reads "fine", +-2.5 dB "look here"

juce::String embedded (const char* fileName)
{
    int size = 0;
    for (int i = 0; i < ProductData::namedResourceListSize; ++i)
        if (juce::String (ProductData::getNamedResourceOriginalFilename (ProductData::namedResourceList[i])) == fileName)
            if (const auto* data = ProductData::getNamedResource (ProductData::namedResourceList[i], size))
                return juce::String::fromUTF8 (data, size);
    return {};
}

StripEditor::Info infoFor()
{
    StripEditor::Info info;
    info.productName = JucePlugin_Name;
    info.version = PRODUCT_VERSION;
    info.what = "Compares your mix with a reference at matched loudness, band by band.";
    info.ownLine = "A bounce is always your mix, untouched.";
    info.licences = embedded ("LICENSE") + "\n\n" + embedded ("THIRD_PARTY_NOTICES.md");
    info.soundVersion = absee::State::kDspVersion;
    return info;
}

juce::StringArray choicesOf (ABSeeAudioProcessor& p, const char* id)
{
    if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p.parameters().getParameter (id)))
        return c->choices;
    return {};
}

void attachSegment (SegmentedControl& s, ABSeeAudioProcessor& p, const char* id)
{
    if (auto* param = p.parameters().getParameter (id))
        s.attach (*param);
    s.setTooltip (Params::tooltipFor (id));
}

bool isAudioFile (const juce::String& path)
{
    for (auto ext : { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3", ".m4a", ".caf" })
        if (path.endsWithIgnoreCase (ext))
            return true;
    return false;
}

juce::String signedDb (float db, bool withSign)
{
    const auto magnitude = juce::String (std::abs (db), 1);
    if (! withSign)
        return magnitude;
    if (std::abs (db) < 0.05f)
        return "0.0";
    return (db > 0.0f ? "+" : "-") + magnitude;
}
} // namespace

//==============================================================================
/** The eight slots: number, name, the active one's LED. Click selects (the Reference
    parameter), double-click or Enter opens the file chooser, up and down move. */
class ABSeeAudioProcessorEditor::SlotList final : public juce::Component
{
public:
    explicit SlotList (ABSeeAudioProcessorEditor& e) : editor (e)
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (false);   // the ring is for the keyboard, not after a click
        setTitle ("References");
        setDescription ("Eight reference slots. Click to listen to one, double-click to choose its file.");
    }

    void update (const std::array<SlotView, absee::kNumSlots>& next, int nextActive, bool nextBlind)
    {
        bool same = nextActive == active && nextBlind == blind;
        for (size_t i = 0; i < views.size() && same; ++i)
            same = views[i].name == next[i].name && views[i].problem == next[i].problem && views[i].loading == next[i].loading;
        if (same)
            return;
        views = next;
        active = nextActive;
        blind = nextBlind;
        repaint();
    }

    int rowAt (int y) const { return juce::jlimit (0, absee::kNumSlots - 1, y / kRowHeight); }
    juce::Rectangle<int> rowBounds (int i) const { return { 0, i * kRowHeight, getWidth(), kRowHeight }; }

    void paint (juce::Graphics& g) override
    {
        const auto& t = front();
        for (int i = 0; i < absee::kNumSlots; ++i)
        {
            const auto& v = views[(size_t) i];
            auto row = rowBounds (i).toFloat();
            if (i == active)
            {
                g.setColour (t.hover);
                g.fillRoundedRectangle (row, Tokens::radiusSm);
            }
            auto inner = row.reduced ((float) Tokens::space2, 0.0f);
            type::draw (g, juce::String (i + 1), inner.removeFromLeft (16.0f), type::meta(), t.inkMuted);
            inner.removeFromLeft ((float) Tokens::space1);
            const auto led = inner.removeFromRight (8.0f).withSizeKeepingCentre (8.0f, 8.0f);
            if (v.name.isNotEmpty())   // an LED only where there is something to light
                drawLed (g, led, i == active && v.problem.isEmpty(), t);
            inner.removeFromRight ((float) Tokens::space2);

            juce::String text = v.name;
            auto colour = t.ink;
            if (v.name.isEmpty())
            {
                text = "Empty";
                colour = t.inkSubtle;
            }
            else if (v.problem.isNotEmpty())
            {
                text = "Cannot load: " + (blind ? juce::String ("hidden") : v.name);
                colour = t.clip;
            }
            else if (blind)
            {
                text = "Name hidden";
                colour = t.inkMuted;
            }
            else if (v.loading && i == active)
            {
                text = v.name + "  (loading)";
            }
            type::draw (g, text, inner, i == active ? type::controlStrong() : type::control(), colour);
        }
        if (hasKeyboardFocus (false))
            drawFocusRing (g, rowBounds (active).toFloat(), Tokens::radiusSm, t);
    }

    void mouseDown (const juce::MouseEvent& e) override { editor.selectSlot (rowAt (e.y)); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { editor.chooseReference (rowAt (e.y)); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::upKey)   { editor.selectSlot (juce::jmax (0, active - 1)); return true; }
        if (key == juce::KeyPress::downKey) { editor.selectSlot (juce::jmin (absee::kNumSlots - 1, active + 1)); return true; }
        if (key == juce::KeyPress::returnKey) { editor.chooseReference (active); return true; }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    ABSeeAudioProcessorEditor& editor;
    std::array<SlotView, absee::kNumSlots> views {};
    int active = 0;
    bool blind = false;
};

//==============================================================================
/** Defining feature 3, the hero: seven bars from a centre line, the signed number above each
    label. Blind hides the sign: the bars then stand symmetric about the line (how far apart,
    not which way), so they cannot tell which side is X. */
class ABSeeAudioProcessorEditor::DeltaChart final : public juce::Component
{
public:
    DeltaChart()
    {
        setTitle ("Band deltas");
        setDescription ("Mix minus reference per band, in dB, level matched.");
    }

    void update (const absee::BandDeltaReading& next, bool nextBlind)
    {
        bool same = nextBlind == blind;
        for (size_t b = 0; b < (size_t) absee::kNumBands && same; ++b)
            same = next.valid[b] == reading.valid[b] && std::abs (next.deltaDb[b] - reading.deltaDb[b]) < 0.05f;
        if (same)
            return;
        reading = next;
        blind = nextBlind;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = front();
        auto area = getLocalBounds().toFloat();
        auto labels = area.removeFromBottom (16.0f);
        auto values = area.removeFromBottom (18.0f);
        area.removeFromBottom ((float) Tokens::space2);

        g.setColour (t.surfaceSunken);
        g.fillRoundedRectangle (area, Tokens::radiusMd);
        auto plot = area.reduced ((float) Tokens::space3, (float) Tokens::space3);
        auto gutter = plot.removeFromLeft (32.0f);
        const auto centreY = plot.getCentreY();
        const auto yFor = [&] (float db) { return centreY - juce::jlimit (-1.0f, 1.0f, db / kChartRangeDb) * plot.getHeight() * 0.5f; };

        bool anyValid = false;
        for (auto v : reading.valid)
            anyValid = anyValid || v;
        if (! anyValid)
        {
            for (int b = 0; b < absee::kNumBands; ++b)
                type::draw (g, absee::kBandNames[(size_t) b],
                            juce::Rectangle<float> (plot.getX() + plot.getWidth() / (float) absee::kNumBands * (float) b, labels.getY(),
                                                    plot.getWidth() / (float) absee::kNumBands, labels.getHeight()),
                            type::label(), t.inkMuted, juce::Justification::centred);
            type::draw (g, "Add a reference to compare the bands", area, type::small(), t.inkSubtle, juce::Justification::centred);
            return;
        }

        // the guides: "look here" beyond +-2.5 dB, the centre line strong
        g.setColour (t.line);
        for (float db : { kLookHereDb, -kLookHereDb })
            g.fillRect (plot.getX(), yFor (db), plot.getWidth(), 1.0f);
        g.setColour (t.lineStrong);
        g.fillRect (plot.getX(), centreY - 0.5f, plot.getWidth(), 1.0f);
        for (float db : { kLookHereDb, 0.0f, -kLookHereDb })
            type::draw (g, db > 0.0f ? "+" + juce::String (db, 1) : juce::String (db, db < 0.0f ? 1 : 0),
                        { gutter.getX(), yFor (db) - 6.0f, gutter.getWidth() - 4.0f, 12.0f }, type::meta(), t.inkSubtle,
                        juce::Justification::centredRight);

        const auto column = plot.getWidth() / (float) absee::kNumBands;
        for (int b = 0; b < absee::kNumBands; ++b)
        {
            const auto x = plot.getX() + column * (float) b;
            auto colBox = juce::Rectangle<float> (x, values.getY(), column, values.getHeight());
            type::draw (g, absee::kBandNames[(size_t) b], juce::Rectangle<float> (x, labels.getY(), column, labels.getHeight()),
                        type::label(), t.inkMuted, juce::Justification::centred);
            if (! reading.valid[(size_t) b])
                continue;
            const auto d = reading.deltaDb[(size_t) b];
            const auto barWidth = juce::jmin (kBarWidth, column * 0.5f);   // the length reads, not the area (rule 6: glow <= 2 %)
            juce::Rectangle<float> bar;
            if (blind)
            {
                const auto half = (centreY - yFor (std::abs (d))) * 0.5f;
                bar = { x + (column - barWidth) * 0.5f, centreY - half, barWidth, juce::jmax (1.0f, 2.0f * half) };
            }
            else
            {
                const auto y = yFor (d);
                bar = juce::Rectangle<float>::leftTopRightBottom (x + (column - barWidth) * 0.5f, juce::jmin (y, centreY),
                                                                  x + (column + barWidth) * 0.5f, juce::jmax (y, centreY));
                bar.setHeight (juce::jmax (1.0f, bar.getHeight()));
            }
            g.setColour (Tokens::accent());
            g.fillRoundedRectangle (bar, Tokens::radiusXs * 0.5f);
            type::draw (g, signedDb (d, ! blind), colBox, std::abs (d) >= kLookHereDb ? type::controlStrong() : type::value(),
                        t.ink, juce::Justification::centred);
        }
    }

private:
    absee::BandDeltaReading reading {};
    bool blind = false;
};

//==============================================================================
/** The match readout (defining feature 2) on the charcoal readout bubble: the side in
    on-readout, the value in the accent (4.9:1 on #1C1B19, sketch 2.3). */
class ABSeeAudioProcessorEditor::MatchReadout final : public juce::Component
{
public:
    MatchReadout() { setTitle ("Match"); }

    void update (const juce::String& next)
    {
        if (next == text)
            return;
        text = next;
        setDescription (text);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = front();
        auto box = getLocalBounds().toFloat();
        g.setColour (t.readout);
        g.fillRoundedRectangle (box, Tokens::radiusMd);
        auto inner = box.reduced ((float) Tokens::space4, 0.0f);
        // "Reference +6.0 dB": the words in on-readout, the signed value in the accent
        const auto valueStart = text.indexOfAnyOf ("+-");
        const auto font = type::font (type::Weight::semibold, 15.0f, 0.0f, true);
        if (valueStart > 0)
        {
            const auto words = text.substring (0, valueStart);
            const auto value = text.substring (valueStart);
            type::draw (g, words, inner, font, t.onReadout);
            inner.removeFromLeft (type::width (font, words));
            type::draw (g, value, inner, font, Tokens::accent());
        }
        else
        {
            type::draw (g, text, inner, font, t.onReadout);
        }
    }

private:
    juce::String text;
};

//==============================================================================
/** The Section parameter as chips (sketch 2.3: "the chosen section chip"): six options are
    one more than a segmented control holds, so they wrap. The chosen chip is the accent with
    on-accent text; the rest are outlined. Click or the arrow keys choose. */
class ABSeeAudioProcessorEditor::SectionChips final : public juce::Component,
                                                      public juce::SettableTooltipClient
{
public:
    SectionChips (juce::RangedAudioParameter& p, const juce::StringArray& labels)
        : options (labels),
          attachment (p, [this] (float v) { selected = juce::jlimit (0, options.size() - 1, (int) std::lround (v)); repaint(); })
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (false);
        setTitle (p.getName (64));
        attachment.sendInitialUpdate();
    }

    /** The height the chips need at `width`. */
    int heightFor (int width) const
    {
        const auto rects = layout (width);
        return rects.empty() ? 0 : (int) std::ceil (rects.back().getBottom());
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = front();
        const auto rects = layout (getWidth());
        for (int i = 0; i < options.size(); ++i)
        {
            const auto r = rects[(size_t) i];
            const bool on = i == selected;
            g.setColour (on ? Tokens::accent() : t.surfaceSunken);
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            if (! on)
            {
                g.setColour (t.lineControl);
                g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
            }
            type::draw (g, options[i], r, on ? type::controlStrong() : type::control(), on ? Tokens::onAccent() : t.ink,
                        juce::Justification::centred);
            if (on && hasKeyboardFocus (false))
                drawFocusRing (g, r, r.getHeight() * 0.5f, t);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto rects = layout (getWidth());
        for (int i = 0; i < (int) rects.size(); ++i)
            if (rects[(size_t) i].contains (e.position))
                choose (i);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey)    { choose (juce::jmax (0, selected - 1)); return true; }
        if (key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey) { choose (juce::jmin (options.size() - 1, selected + 1)); return true; }
        return false;
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    static constexpr float kChipHeight = 24.0f, kGap = 6.0f, kPad = 12.0f;

    std::vector<juce::Rectangle<float>> layout (int width) const
    {
        std::vector<juce::Rectangle<float>> out;
        float x = 0.0f, y = 0.0f;
        for (const auto& o : options)
        {
            const auto w = juce::jmax (kChipHeight + 4.0f, type::width (type::controlStrong(), o) + 2.0f * kPad);
            if (x > 0.0f && x + w > (float) width)
            {
                x = 0.0f;
                y += kChipHeight + kGap;
            }
            out.emplace_back (x, y, w, kChipHeight);
            x += w + kGap;
        }
        return out;
    }

    void choose (int i)
    {
        attachment.setValueAsCompleteGesture ((float) i);
    }

    juce::StringArray options;
    int selected = 0;
    juce::ParameterAttachment attachment;
};

//==============================================================================
ABSeeAudioProcessorEditor::ABSeeAudioProcessorEditor (ABSeeAudioProcessor& p)
    : StripEditor (p, p.presetHost(), infoFor()),
      audioProcessor (p),
      listen (choicesOf (p, Params::listen)),
      listenBlind (juce::StringArray { "X", "Y" }),
      match (choicesOf (p, Params::matchMode)),
      blind ("Blind"), mono ("Mono"), follow ("Follow transport"), trimMixInstead ("Trim mix instead"),
      addReference ("Add reference...", ActionButton::Variant::secondary),
      clearReference ("Clear", ActionButton::Variant::ghost),
      preferX ("Prefer X", ActionButton::Variant::secondary),
      preferY ("Prefer Y", ActionButton::Variant::secondary),
      noDifference ("No difference", ActionButton::Variant::ghost),
      revealButton ("Reveal", ActionButton::Variant::ghost),
      trim (p.parameters(), Params::trim, "TRIM", Tokens::knobLg),
      matchTarget (p.parameters(), Params::matchTarget, "TARGET", Tokens::knobMd),
      meter (p.outputPeaks(), LevelMeter::Led::clip)
{
    auto& apvts = audioProcessor.parameters();

    slots = std::make_unique<SlotList> (*this);
    chart = std::make_unique<DeltaChart>();
    readout = std::make_unique<MatchReadout>();
    addAndMakeVisible (*slots);
    addAndMakeVisible (*chart);
    addAndMakeVisible (*readout);

    attachSegment (listen, p, Params::listen);
    attachSegment (listenBlind, p, Params::listen);
    attachSegment (match, p, Params::matchMode);
    for (auto* s : { &listen, &listenBlind, &match })
        addAndMakeVisible (*s);
    section = std::make_unique<SectionChips> (*apvts.getParameter (Params::section), choicesOf (p, Params::section));
    section->setTooltip (Params::tooltipFor (Params::section));
    addAndMakeVisible (*section);
    listenBlind.setVisible (false);

    const std::pair<PillToggle*, const char*> pills[] = {
        { &blind, Params::blind }, { &mono, Params::mono }, { &follow, Params::followTransport },
        { &trimMixInstead, Params::trimMixInstead } };
    for (auto [pill, id] : pills)
    {
        pill->setTooltip (Params::tooltipFor (id));
        pillAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id, *pill));
        addAndMakeVisible (*pill);
    }

    for (auto* k : { &trim, &matchTarget })
    {
        const auto id = k == &trim ? Params::trim : Params::matchTarget;
        k->knob().setTooltip (Params::tooltipFor (id));
        k->knob().setHelpText (Params::tooltipFor (id));
        addAndMakeVisible (*k);
    }
    addAndMakeVisible (meter);

    addReference.setTooltip ("Choose an audio file for the selected slot; you can also drop one on the list.");
    clearReference.setTooltip ("Empties the selected slot; the file itself is not touched.");
    addReference.onClick = [this] { chooseReference (activeSlot()); };
    clearReference.onClick = [this] { setReference (activeSlot(), {}); };
    preferX.onClick = [this] { vote (0); };
    preferY.onClick = [this] { vote (1); };
    noDifference.onClick = [this] { vote (2); };
    revealButton.onClick = [this] { reveal(); };
    for (auto* b : { &addReference, &clearReference, &preferX, &preferY, &noDifference, &revealButton })
        addAndMakeVisible (*b);

    setBodySize (kBodyWidth, kBodyHeight);
    audioProcessor.presetLibrary().writeFactoryFolder();   // the bank as files, once per version
}

ABSeeAudioProcessorEditor::~ABSeeAudioProcessorEditor() = default;

//==============================================================================
void ABSeeAudioProcessorEditor::layoutBody (juce::Rectangle<int> area)
{
    // OUTPUT, last on the right
    // wide enough for the longest value ("-30.0 LUFS"): ParameterKnob's preferred width covers the
    // knob and its label, not its value text
    const auto outputWidth = juce::jmax (kOutputWidth, trim.getPreferredWidth(), meter.getPreferredWidth(), matchTarget.getPreferredWidth());
    auto output = area.removeFromRight (outputWidth);
    area.removeFromRight (kGroupGap);
    ruleRight = area.removeFromRight (1);
    area.removeFromRight (kGroupGap);

    outputTitle = output.removeFromTop (kTitleHeight);
    output.removeFromTop (Tokens::space3);
    trim.setBounds (output.removeFromTop (trim.getPreferredHeight()));
    output.removeFromTop (Tokens::space4);
    meter.setBounds (output.removeFromTop (LevelMeter::kHeight).withSizeKeepingCentre (meter.getPreferredWidth(), LevelMeter::kHeight));
    output.removeFromTop (Tokens::space5);
    matchTarget.setBounds (output.removeFromTop (matchTarget.getPreferredHeight()));

    // REFERENCES, first on the left
    auto refs = area.removeFromLeft (kReferencesWidth);
    area.removeFromLeft (kGroupGap);
    ruleLeft = area.removeFromLeft (1);
    area.removeFromLeft (kGroupGap);

    referencesTitle = refs.removeFromTop (kTitleHeight);
    refs.removeFromTop (Tokens::space3);
    slots->setBounds (refs.removeFromTop (kRowHeight * absee::kNumSlots));
    refs.removeFromTop (Tokens::space3);
    {
        auto row = refs.removeFromTop (kRowHeight);
        addReference.setBounds (row.removeFromLeft (addReference.getPreferredWidth()));
        row.removeFromLeft (Tokens::space2);
        clearReference.setBounds (row.removeFromLeft (clearReference.getPreferredWidth()));
    }
    refs.removeFromTop (kGroupGap);
    sectionTitle = refs.removeFromTop (kTitleHeight);
    refs.removeFromTop (Tokens::space3);
    section->setBounds (refs.removeFromTop (section->heightFor (refs.getWidth())));
    refs.removeFromTop (Tokens::space2);
    sectionCaption = refs.removeFromTop (16);

    // LISTEN, then BALANCE (the hero) below it
    listenTitle = area.removeFromTop (kTitleHeight);
    area.removeFromTop (Tokens::space3);
    {
        auto row = area.removeFromTop (kListenHeight);
        const auto switchWidth = 200;
        listen.setBounds (row.removeFromLeft (switchWidth));
        listenBlind.setBounds (listen.getBounds());
        row.removeFromLeft (Tokens::space4);
        readout->setBounds (row);
    }
    area.removeFromTop (Tokens::space3);
    {
        auto row = area.removeFromTop (kRowHeight);
        match.setBounds (row.removeFromLeft (match.getPreferredWidth()));
        row.removeFromLeft (Tokens::space4);
        blind.setBounds (row.removeFromLeft (blind.getPreferredWidth()));
    }
    area.removeFromTop (Tokens::space2);
    {
        auto row = area.removeFromTop (kRowHeight);
        for (auto* pill : { &mono, &follow, &trimMixInstead })
        {
            pill->setBounds (row.removeFromLeft (pill->getPreferredWidth()));
            row.removeFromLeft (Tokens::space2);
        }
    }
    area.removeFromTop (Tokens::space2);
    {
        auto row = area.removeFromTop (kRowHeight);
        for (auto* b : { &preferX, &preferY, &noDifference, &revealButton })
        {
            b->setBounds (row.removeFromLeft (b->getPreferredWidth()));
            row.removeFromLeft (Tokens::space2);
        }
    }
    area.removeFromTop (Tokens::space5);
    balanceTitle = area.removeFromTop (kTitleHeight);
    area.removeFromTop (Tokens::space3);
    balanceCaption = area.removeFromBottom (16);
    area.removeFromBottom (Tokens::space2);
    chart->setBounds (area);
}

void ABSeeAudioProcessorEditor::paint (juce::Graphics& g)
{
    StripEditor::paint (g);
    const auto& t = front();
    type::draw (g, "REFERENCES", referencesTitle.toFloat(), type::section(), t.inkMuted);
    type::draw (g, "SECTION", sectionTitle.toFloat(), type::section(), t.inkMuted);
    type::draw (g, "LISTEN", listenTitle.toFloat(), type::section(), t.inkMuted);
    type::draw (g, "BALANCE", balanceTitle.toFloat(), type::section(), t.inkMuted);
    type::draw (g, "OUTPUT", outputTitle.toFloat(), type::section(), t.inkMuted);
    type::draw (g, sectionText, sectionCaption.toFloat(), type::meta(), t.inkSubtle);
    type::draw (g, "Mix minus reference per band, in dB, after the match", balanceCaption.toFloat(), type::meta(), t.inkSubtle);
    g.setColour (t.line);
    g.fillRect (ruleLeft);
    g.fillRect (ruleRight);
}

//==============================================================================
int ABSeeAudioProcessorEditor::activeSlot() const
{
    if (auto* v = audioProcessor.parameters().getRawParameterValue (Params::refSlot))
        return juce::jlimit (0, absee::kNumSlots - 1, (int) std::lround (v->load()));
    return 0;
}

void ABSeeAudioProcessorEditor::selectSlot (int slot)
{
    if (auto* p = audioProcessor.parameters().getParameter (Params::refSlot))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) slot));
        p->endChangeGesture();
    }
}

void ABSeeAudioProcessorEditor::chooseReference (int slot)
{
    selectSlot (slot);
    chooser = std::make_unique<juce::FileChooser> ("Choose a reference for slot " + juce::String (slot + 1),
                                                   oberli::references::folder(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, slot] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  setReference (slot, file);
                          });
}

void ABSeeAudioProcessorEditor::setReference (int slot, const juce::File& file)
{
    // A new file has new sections: the marks and the align pair start empty (the loader picks
    // the path up on its next tick and reports a file it cannot read).
    auto& s = audioProcessor.slotState (slot);
    s = absee::SlotState {};
    if (file != juce::File())
        s.path = file.getFullPathName();
    selectSlot (slot);
}

bool ABSeeAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return StripEditor::isInterestedInFileDrag (files);
}

void ABSeeAudioProcessorEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::StringArray audio, others;
    for (const auto& f : files)
        (isAudioFile (f) ? audio : others).add (f);
    if (! others.isEmpty())
        StripEditor::filesDropped (others, x, y);
    // onto a row: that slot and the ones after it; anywhere else: the selected slot onwards
    const auto local = slots->getLocalPoint (this, juce::Point<int> (x, y));
    int slot = slots->getLocalBounds().contains (local) ? slots->rowAt (local.y) : activeSlot();
    const int first = slot;
    for (const auto& f : audio)
        if (slot < absee::kNumSlots)
            setReference (slot++, juce::File (f));
    if (! audio.isEmpty())
        selectSlot (first);
}

//==============================================================================
void ABSeeAudioProcessorEditor::vote (int choice)
{
    const bool xIsMix = audioProcessor.blindRound().xIsMix();
    if (choice == 2)
        ++votesSame;
    else if ((choice == 0) == xIsMix)
        ++votesMix;
    else
        ++votesReference;
    revealed = false;
    audioProcessor.blindRound().newRound();   // the next round draws X and Y again
}

void ABSeeAudioProcessorEditor::reveal()
{
    revealed = true;
}

juce::String ABSeeAudioProcessorEditor::readoutText() const
{
    if (audioProcessor.renderWireForced())
        return "Rendering: mix passed through";
    const auto& s = audioProcessor.slotState (activeSlot());
    if (s.isEmpty())
        return "No reference in slot " + juce::String (activeSlot() + 1);
    const bool isBlind = audioProcessor.parameters().getRawParameterValue (Params::blind)->load() >= 0.5f;
    if (isBlind && revealed)
    {
        const auto x = juce::String (audioProcessor.blindRound().xIsMix() ? "Mix" : "Reference");
        const auto lead = votesMix == votesReference ? juce::String ("even")
                                                     : (votesMix > votesReference ? juce::String ("for Mix") : juce::String ("for Reference"));
        return "X was " + x + " - " + juce::String (juce::jmax (votesMix, votesReference)) + " : "
             + juce::String (juce::jmin (votesMix, votesReference)) + " " + lead;
    }
    return audioProcessor.matchReadout();
}

//==============================================================================
void ABSeeAudioProcessorEditor::advanceBody (double now, float dt)
{
    trim.advance (now, dt);
    matchTarget.advance (now, dt);
    meter.advance (now, dt);

    // The mix's band analysis: the processor's timer drains it too; draining here as well keeps
    // the display current where no message loop runs (the snapshot tool). Serialised inside.
    audioProcessor.mixBands().drain();

    auto& apvts = audioProcessor.parameters();
    const bool isBlind = apvts.getRawParameterValue (Params::blind)->load() >= 0.5f;
    const bool trimsMix = apvts.getRawParameterValue (Params::trimMixInstead)->load() >= 0.5f;
    const bool rendering = audioProcessor.renderWireForced();
    const int active = activeSlot();

    if (isBlind != lastBlind)
    {
        lastBlind = isBlind;
        votesMix = votesReference = votesSame = 0;
        revealed = false;
        listen.setVisible (! isBlind);
        listenBlind.setVisible (isBlind);
    }
    for (auto* b : { &preferX, &preferY, &noDifference, &revealButton })
        b->setVisible (isBlind);

    std::array<SlotView, absee::kNumSlots> views;
    for (int i = 0; i < absee::kNumSlots; ++i)
    {
        const auto& s = audioProcessor.slotState (i);
        auto& v = views[(size_t) i];
        if (s.isEmpty())
            continue;
        v.name = juce::File (s.path).getFileNameWithoutExtension();
        v.problem = audioProcessor.referenceLoader().errorFor (i);
        v.loading = audioProcessor.referenceStore().acquire (i) == nullptr && v.problem.isEmpty();
    }
    slots->update (views, active, isBlind);

    const bool haveReference = views[(size_t) active].name.isNotEmpty() && views[(size_t) active].problem.isEmpty();
    for (auto* s : { &listen, &listenBlind })
    {
        // SegmentedControl draws no disabled state of its own: the brand's 45 % (the Knob card's)
        s->setEnabled (haveReference && ! rendering);
        s->setAlpha (s->isEnabled() ? 1.0f : 0.45f);
    }
    clearReference.setEnabled (views[(size_t) active].name.isNotEmpty());
    matchTarget.setEnabled (trimsMix);

    readout->update (readoutText());
    chart->update (audioProcessor.bandDeltas(), isBlind);

    juce::String nextSection;
    if (audioProcessor.referenceIsPlaying())
    {
        const auto fmt = [] (double s) { return juce::String ((int) s / 60) + ":" + juce::String (std::fmod (s, 60.0), 1).paddedLeft ('0', 4); };
        nextSection = "Playing " + fmt (audioProcessor.activeSectionIn()) + " to " + fmt (audioProcessor.activeSectionOut());
    }
    if (nextSection != sectionText)
    {
        sectionText = nextSection;
        repaint (sectionCaption);
    }
}

void ABSeeAudioProcessorEditor::presetRecalled (double now)
{
    trim.knob().beginWave (now, false);
    matchTarget.knob().beginWave (now + 0.04, false);
}

bool ABSeeAudioProcessorEditor::previewDrag (const juce::String& parameterId)
{
    if (parameterId == Params::trim)        { trim.knob().setDragPreview (true); return true; }
    if (parameterId == Params::matchTarget) { matchTarget.knob().setDragPreview (true); return true; }
    return false;
}
