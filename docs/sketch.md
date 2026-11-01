# A B see - loudness-matched reference A/B (Tier S sketch)

Tier S, gate G0-G2 sketch. Oberli Audio effect, Bone theme, accent jade #3FCF9E (check the Design System artifact that jade is still unassigned; Horizon Pad holds mango). Static frame: `a-b-see@1x.png` (960 x 600 at 100 %), `a-b-see@2x.png` for Retina viewing. Sound side: sound-design-bob is needed for the loudness measurement, the gain smoothing and the file streaming; this sketch states what the DSP must do, not how.

## 1. G0 brief

**Users.** Björn mixing and mastering worship recordings in Ableton Live on macOS arm64 and Windows; friends who get the build. Secondary: anyone who mixes in Live and owns neither Metric AB nor REFERENCE.

**Core jobs.**
1. Hear my mix against a commercial reference at exactly the same loudness, switching without a click and without touching the DAW.
2. Compare the right sections (my chorus against their chorus), looped, in sync with my transport or free-running.
3. See what the ear cannot judge in 10 s: how far off I am per band, in width, in dynamics, and by how many dB the reference had to be trimmed.
4. Remove my own bias: blind X/Y with a tally.
5. Never ruin a bounce: the plugin is bit-transparent on export whatever state it was left in.

**Context.** Last device on the master, after the limiter. Window open for minutes, not hours. One hand on the switch, eyes on the band deltas. Often used at -18 dBFS monitoring level with the DAW master at unity.

**Success.** Mix-to-reference switch is level matched within 0.1 LU and click-free; the bounce of a project that was left in "Reference" mode is identical to a bounce with the plugin bypassed (null test); a friend sets up a reference section in under a minute without the manual.

**The five core controls.** Listen (Mix / Reference), Reference slot (1-8), Match (Integrated / Short 3 s / True peak / Off), Blind, Trim. Everything else is editor state or lives in the drawer.

**CPU and latency budget.** Zero latency reported to the host in every mode. Idle with editor open: 1 % or less. Playing a reference with the analyser running: 3 % on an M1 at 48 kHz, 128 samples. Reference files are decoded once on load into RAM (float32, resampled to the session rate on load, not per block).

**Three structural decisions (sound side).**
1. The mix path is a wire. In Mix mode no gain stage sits in the signal; "Trim mix instead" inserts a single smoothed gain and is off by default.
2. Loudness matching is measured, not guessed: reference loudness is computed offline on load (EBU R128 integrated over the whole file and over each section); mix loudness is measured live on the same meter the readout shows. Matching gain = measured difference, applied to the reference only.
3. Offline render = Mix mode, unity, always. `isNonRealtime()` plus the host's offline flag force the wire path; the editor shows "Rendering: mix passed through" when it notices.

## 2. G1 design plan (five lines)

1. **Window** 960 x 600 at 100 %, Bone, compact: Bone glares beside a dark DAW, so the analyser well stays the only large sunken area and nothing goes brighter than `surface-raised`.
2. **Hero** is what the ear cannot judge: the seven band deltas and the match readout "Reference -1.4 dB". The Listen switch is big because the hand needs it, but it carries no information.
3. **Accent** jade, only on what is active: the selected Listen segment, lit LEDs, the chosen section chip, the reference curve, the delta bars, the knob pointer and the readout's delta value (on Bone the readout is the charcoal bubble, so accent text is allowed there, 4.9:1 against #1C1B19).
4. **Sections** from the parameter table's group order: References, Listen, Balance, Output. Output column last on the right with Trim at 64 px and the LUFS S meter; no Mix knob because this plugin has no wet path (one line to add to the brand sheet: utility plugins without a wet path omit Mix; the output column holds their output knob and meter).
5. **States**: modified (preset pill dims when the match mode or slot differs from the preset), drawer (preset drawer from the left, 380 px), About (charcoal page behind the effect), bypass (hard bypass = wire), rendering (header shows "Rendering" in `ink-subtle`, Listen segment greyed), blind (Listen reads X / Y, slot names hidden, tally in the readout), no reference loaded (Listen segment disabled, "Drop a file here" row stays visible).

## 3. Parameter table (sketch, six columns)

Host order = this order; the core controls are the first five so Ableton Live 10.1 shows them in its device panel.

| id | type | name | group | unit / choices | min / max / default |
|---|---|---|---|---|---|
| listen | choice | Listen | Listen | Mix \| Reference | 0 / 1 / 0 |
| refSlot | int | Reference | Listen | 1-8 | 1 / 8 / 1 |
| matchMode | choice | Match | Listen | Integrated \| Short 3 s \| True peak \| Off | 0 / 3 / 0 |
| blind | bool | Blind | Listen | - | 0 / 1 / 0 |
| trim | float | Trim | Output | dB, bipolar | -24 / +24 / 0 |
| mono | bool | Mono | Listen | - | 0 / 1 / 0 |
| followTransport | bool | Follow transport | Listen | - | 0 / 1 / 1 |
| trimMixInstead | bool | Trim mix instead | Listen | - | 0 / 1 / 0 |
| section | choice | Section | References | Whole \| 1 \| 2 \| 3 \| 4 \| Loudest 10 s | 0 / 5 / 5 |
| matchTarget | float | Match target | Listen | LUFS | -30 / -6 / -14 |
| bypass | bool | Bypass | - | - | 0 / 1 / 0 |

Not host parameters (editor and plugin state, saved in the project): reference file paths and their cached loudness, section markers per reference, blind tally, analyser settings (averaging, 1/3 or 1/6 octave), monitoring-level readout preference.

`matchTarget` is only used when `trimMixInstead` is on: then both mix and reference are trimmed to the target so a -8 LUFS master and a -14 LUFS reference meet at a sane monitoring level. With it off, the reference is trimmed to the mix, which is what the Listen switch promises.

Tooltips (one sentence of sonic effect; the generator appends range and default):
- Listen: "Switches the output between your mix and the chosen reference at matched loudness."
- Match: "How the two are brought to the same loudness before you switch; Off keeps raw levels."
- Blind: "Hides which side is which and keeps a tally of your votes."
- Trim: "Monitoring level for both sides; does not touch a bounce."
- Trim mix instead: "Trims your mix to the match target too, so a loud master and a quiet reference meet in the middle."
- Follow transport: "Plays the reference section in step with the DAW position; off loops it freely."

## 4. Approach and best practice (the documentation of the desired tool)

### 4.1 Signal path
```
in ──┬──────────────────────────────────────────── (wire) ──┐
     │                                                       ├─ crossfade 5 ms ─ Trim ─ out
     └─ meter (LUFS I/S, TP, PSR, spectrum, width)           │
reference RAM ─ section loop / transport map ─ gain (match) ─ meter ─┘
```
- The switch is a 5 ms equal-power crossfade, started at the next block boundary; no zero-crossing search (it adds latency and the fade already hides the click).
- The match gain is a one-pole smoother with 20 ms time constant so a changing short-term match does not pump; in Integrated mode it only changes when the measurement changes by more than 0.1 LU.
- Trim sits after the crossfade and before the output, so it is the one gain both sides share; it is reported to the host as a parameter so a controller can ride it.
- Mono sums at -3 dB after the crossfade (same on both sides).

### 4.2 Loudness matching, three modes and Off
- **Integrated** (default): reference LUFS-I of the active section (offline, ITU-R BS.1770-4 gated) against the mix's integrated loudness since play started in this section (reset on transport stop or section change). Robust, no pumping, right for "is my master as loud as theirs".
- **Short 3 s**: sliding LUFS-S of both, smoothed; right for comparing a quiet verse where integrated numbers mislead. The readout shows the current trim and the user sees it move.
- **True peak**: aligns the maximum true peak of the section (4x oversampled, BS.1770 annex 2); right for judging limiter density and punch, which loudness matching hides.
- **Off**: raw levels, trim 0 dB, for checking the actual delivered level.
- Whichever mode, the readout says what was done, in dB, with sign and the word Reference or Mix, never a bare number.

### 4.3 References and sections
- Up to 8 slots, drag and drop onto the list or "Add reference…". Decoded via JUCE's `AudioFormatManager` (WAV, AIFF, FLAC, MP3, AAC through the OS codecs) into float32 at session rate; a 5 min stereo file at 48 kHz is 115 MB, so slots load lazily and the plugin keeps at most two decoded (the active one and the last one) and the rest as paths with cached analysis.
- Sections per reference: four user markers (in/out) plus "Loudest 10 s" (the 10 s window with the highest short-term loudness, found on load). Markers are set by dragging on the waveform strip or typed; number keys 1-4 switch sections while the editor has focus.
- **Follow transport**: an offset maps the DAW's bar position to a reference time (set once by pressing "Align here" while the DAW stands at your chorus with the reference section selected); after that the reference runs with the transport and loops inside its section. Free-running mode loops the section regardless of the transport, so the reference also plays while the DAW is stopped.
- References and their analysis live in the shared `Oberli Audio/References/` folder (`~/Library/Application Support/`, `%APPDATA%` on Windows), keyed by path, size and mtime, read by A B see and Bench Line alike (decided 2026-11-01), so reopening a project is instant and a reference added in one plugin appears in the other.

### 4.4 Blind X/Y
- Blind on: the Listen segment reads X and Y, the assignment is random per round, the slot name and the reference curve legend are hidden, the band deltas stay (they do not reveal which is which when the sign is hidden; the sketch hides the sign in blind mode).
- Vote buttons "Prefer X" / "Prefer Y" / "No difference" appear under the switch; "Reveal" ends the round, shows the tally in the readout ("X was Mix · 3 : 1 for Mix"). Rounds are editor state, lost on close, by design.

### 4.5 Analysis (the Balance section)
- Spectrum: 1/3 octave (switchable 1/6), 4096 FFT, Hann, averaged over the section loop (not a falling peak display), mix in ink, reference in accent, both drawn after the match gain so the curves are comparable.
- Band deltas: seven bands with fixed edges (Sub < 60, Low 60-150, Lo-mid 150-400, Mid 400-1.5k, Hi-mid 1.5-4k, High 4-10k, Air > 10 k), each the mix's band energy minus the reference's in dB; bars from the centre line, number with sign above the label. These are the hero: a ±1 dB band reads as "fine", ±2.5 dB as "look here".
- Width delta: side/mid energy ratio difference in percent, as a single line in the legend; per-band width is a Tier P addition if the first users ask.
- Numbers table: LUFS I, LUFS S, LRA, TP (dBTP), PSR; mix row live, reference row from cache, delta row signed.
- The display reads data from a lock-free FIFO; a 30 Hz timer, stopped when the transport stands still and the reference loop is off.

### 4.6 Render safety (the rule that makes it usable on a master)
- `prepareToPlay` and every block check `isNonRealtime()`; on true the processor takes the wire path, trim 0 dB, no crossfade, no reference, no metering. Ableton Live's export sets non-realtime; Freeze and Bounce in Place do too.
- Belt and braces: a "Safe on render" item in the menu, on by default, cannot be turned off from a host parameter. The About page and the readme say it in one sentence.
- Test: bounce a 30 s project with the plugin in Reference mode, Blind on, Trim at +6 dB; the bounce nulls against a bounce with the plugin removed. This is the one acceptance test that decides whether the plugin ships at all.

### 4.7 Hosts and quirks (Ableton Live only, per Björn)
- Live 10.1 shows 32 parameters in its panel: the table has 11, all visible.
- Live has no monitor-only insert, so the plugin must be the last device on the master; the readme says so and the editor shows a one-line hint on first run when it detects it is not at the end (not detectable from VST3; the hint is static text in the first-run tooltip overlay instead).
- Live on Windows auto-scales plugin windows: readme note (ship-bob).
- Keyboard shortcuts only while the editor has focus (Live keeps Space and the number keys when the plugin window is not focused; under Rosetta test the focus handover).

### 4.8 What is deliberately not in it
- No EQ matching, no "make my mix sound like the reference" button: A B see tells, it does not fix.
- No streaming-service presets with pass/fail: the numbers table and Youlean cover that.
- No cloud library of references: files only, the user's own.

## 5. Manual outline (ships with the plugin, generated from the table where possible)
1. What it does in one paragraph and the render-safety sentence.
2. Setup: last on the master, add a reference, set a section, choose match mode.
3. The Listen switch and the match readout.
4. Sections and Follow transport, with the "Align here" step.
5. Blind rounds.
6. Reading the Balance section (band deltas, width, the numbers table).
7. Parameters (generated).
8. Troubleshooting: reference silent (file moved, cache stale), level jumps (Short mode on a dynamic section), bounce sounds different (it cannot; here is the null test).

## 6. Acceptance tests for the sketch to be promoted to P
1. Null test on export in every state (4.6).
2. Match accuracy: a reference that is the mix itself at -6 dB reads "Reference +6.0 dB" within 0.1 LU in Integrated and Short modes, 0.1 dB in True peak.
3. Switch click test: a 1 kHz sine on both sides, 200 switches, no sample exceeds the sine's peak by more than 0.01 dB.
4. CPU: editor open, analyser running, reference playing, under 3 % on an M1 at 48 kHz, 128 samples.
5. State recall: reopen a project, the slots, sections, match mode and the last listen state come back; the window scale is machine-local.
6. Memory: 8 slots loaded, at most two decoded buffers resident.

## 7. Open decisions for Björn
- Name: A B see (decided 2026-11-01). Slug `a-b-see`, plugin ID `ABSee`, folders "A B see".
- Accent: jade proposed; confirm against the artifact.
- Should "Trim mix instead" default to on for mastering use? The sketch says off because the promise of the Listen switch is "the reference comes to you".
- Section markers per reference (four) or global (one set for all)? The sketch says per reference; a chorus sits at a different time in every song.
- Tier: P as the default, F if it becomes Björn's daily tool for the worship masters (it probably will).

## 8. Report
```
Tier: S, gate: G0-G2 sketch
Hosts: n/a at S (P: Ableton Live 10.1 under Rosetta, Ableton Live on Windows)
Brand sheet: deviates: output column without Mix knob (utility plugin without wet path), line proposed for the sheet; logo in the frame is a CSS stand-in for the Logo SVG
What works well: hero is information (deltas, match readout), not the switch; the match readout always states what was done and to which side; render safety is a structural decision, not a checkbox
Findings: none at S (hard rules only; no snapshot pass yet)
Open / recommended: name and accent (Schwere 1); per-reference sections (2); Trim-mix default (2)
Evidence: a-b-see@1x.png at 100 %, a-b-see@2x.png
```
