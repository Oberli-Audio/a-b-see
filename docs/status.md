# Status - A B see 0.1.0 (2026-11-01)
Tier P, track A (analysis; stages processing, output, analysis), at G4 closed, G5 part-closed,
G6 blocked on macOS. Recipe D.8.1.

## Waiting for Björn
- 2026-11-01: **`presetlint.py` cannot check Init for this product.** The kit's `check_init`
  compares a *choice* parameter's plain default with the harness's normalised echo, so it
  fails for any choice default but index 0 - here `section` (default Loudest 10 s, index 5).
  Fix belongs in bob-skills' `dsp-kit`, which this repo must not edit. Until then the ctest
  `preset_bank` test runs without `parameters.csv` and the check is
  `test_init_preset_equals_the_table_defaults` in the suite instead, which normalises both
  sides the way the harness does.
- 2026-11-01: **Two template bugs fixed here, worth fixing in `Oberli-Audio/plugin-template`**:
  (1) the harness included `ABSee_artefacts/JuceLibraryCode` for a `JucePluginDefines.h` that
  this JUCE no longer generates (the plugin macros are target compile definitions now), so it
  did not compile at all; (2) the harness sized its work buffer to `--max-block` while
  rendering blocks of `--block`, so the oversized-block test of its own scaffold wrote past
  the end of the buffer (`malloc(): corrupted top size`).
- 2026-11-01: **Shared `Source/oberli/` is not in the repo yet.** A B see needs `Loudness`
  (BS.1770-5 streaming meter: integrated, short-term 3 s, true peak 4x, per-rate K-weighting)
  and `References` (decode, resample on load, the shared `Oberli Audio/References/` cache,
  section markers, offline analysis per section); the editor also needs LookAndFeel, Strip
  frame, meters and Analysis/FFT. Built here instead: the crossfade, the transport map, blind
  mode, render safety and the match policy, each against a seam the shared module plugs into
  (`Source/MixAnalysis.h`, `Source/ReferenceStore.h`). Until it lands the match gain stays
  0.0 dB in Integrated / Short / True peak, so the brief's match-accuracy target cannot be
  measured on the plugin (it is measured on the prototype at G3).
- 2026-11-01: **The switch fades equal-gain, not equal-power as `must_do` says.** Your own
  acceptance test forced it: equal-power on a 1 kHz sine on both sides peaks **+3.0086 dB
  above the sine**, so a switch between two masters at -1 dBTP would clip the monitoring
  path; equal-gain measures +0.0000 dB and costs a 3.01 dB dip in the middle of the 5 ms
  fade when the two sides are uncorrelated (`proto_measure.py --case switch-click`). Built
  equal-gain; say the word if you want the brief's letter instead.
- 2026-11-01: **5 ms crossfade vs rule 2** (switches crossfaded over 10-20 ms). The
  owner-confirmed brief says 5 ms equal-power; it is built as the brief says. Measured at G3
  on the prototype and at G5 on the plugin with the brief's own test (1 kHz sine both sides,
  200 switches): if 5 ms passes it stays, confirm or ask for 10 ms.
- 2026-11-01: **Listening checkpoint** due when the match gain works end to end (needs the
  shared Loudness module): Mix vs Reference on one of your worship masters, Integrated and
  Short 3 s, Blind on.
- 2026-11-01: **Host checks** (Live 10.1 under Rosetta, Live on Windows) cannot run in the
  cloud session; they are the two-host list of the release gate.
- 2026-11-01: **Build slice.** This session builds on Linux/x86_64 with GCC 13 (no macOS, no
  Faust, no pluginval, no RTSan, no Rosetta). Numbers below carry that slice; the goldens,
  `perf.py` rows and the real-time check must be taken again on your Mac
  (`build.arch` in every harness JSON says which slice produced it).

## Gates
| Gate | State | Evidence (command -> number, test name, log) |
|---|---|---|
| G0 | closed 2026-11-01 | `docs/brief.md` owner-confirmed, from `docs/sketch.md` |
| G1 | closed 2026-11-01 | see Decisions (family, trade-off, three departures with their rejecting measurement) |
| G2 | closed 2026-11-01 | `sound design/prototype/absee_proto.py` (numpy, track A: analysis prototypes are numpy, not Faust - F.15.1), one class per C++ module; `python3 "sound design/prototype/absee_proto.py" --demo` -> 4 renders, checked programmatically at G3, **not heard yet** |
| G3 | prototype side closed 2026-11-01, plugin side open | `python3 "sound design/prototype/proto_measure.py" --all` -> 10/10 pass, numbers in the metric table below (JSON: `sound design/prototype/g3-prototype.json`) |
| G4 | closed 2026-11-01 | `cmake --preset release -G Ninja && cmake --build --preset release` -> clean, no warnings in `Source/**`; one module per prototype class, the two shared modules behind seams (`MixAnalysis.h`, `ReferenceStore.h`) |
| G5 | part-closed 2026-11-01 | `test_plugin_matches_the_prototype` -> residual **-108.9 dBFS** after the switch has settled (threshold -90; the first 10 ms are the port's fade-in from Mix and are excluded, measured -32.7 dBFS there). Defining features by name: `test_render_is_always_the_wire` (bit-exact), `test_match_readout_says_what_it_did`. **Open:** the match against a real streaming meter, and feature 3 (band deltas) |
| G6 | part-closed 2026-11-01, rest blocked | `ctest --preset all` -> **5/5 pass** (fast 47 s, render 65 s); `paramgen.py check --list-params --manual docs/parameters.md` -> OK, 11 parameters, round trip at 101 points; `presetlint.py bank --tier P --track effect` -> OK, 9 presets, matches the harness. **Blocked on macOS:** `validate_plugin.py full` (pluginval, Steinberg validator), the malloc trap / RTSan, `perf.py`, `paramgen.py check --frozen` (no release yet). Hosts: Live 10.1 Rosetta -, Live Windows - |
| G7 | - | - |

## Metric table (G3, prototype; build Linux/x86_64, commit at the time of the run)
Every row: `python3 "sound design/prototype/proto_measure.py" --case <case>`; seeded (rule 8),
one run is reproducible bit for bit. Target column is the brief's.

| Case | Target | Measured | Note |
|---|---|---|---|
| `wire-null` | wire, bit-exact | bit-exact (-400 dBFS) | Mix mode at Trim 0 never touches the samples |
| `render-null` | bit-exact | bit-exact (-400 dBFS) | Reference mode, Blind on, Trim +6 dB, Mono on, offline -> the wire (defining feature 1) |
| `match-integrated` | +/-0.1 LU | 0.070 LU | readout "Reference +6.1 dB"; rendered residual against the mix -59.4 dBFS (0.1 dB of gain error would be -38.7) |
| `match-short` | +/-0.1 LU | 0.030 LU | readout "Reference +6.0 dB"; residual -66.8 dBFS |
| `match-truepeak` | +/-0.1 dB | 0.000 dB | readout "Reference +6.0 dB"; residual -331.1 dBFS |
| `match-target` | both sides at the target | 0.007 LU | "Trim mix instead": -8 LUFS mix and -20 LUFS reference both land at -14.0 LUFS |
| `switch-click` | no sample above peak + 0.01 dB | +0.0000 dB | 200 switches, 1 kHz sine both sides, equal-gain 5 ms; equal-power would be **+3.0086 dB** (departure 3) |
| `loop-seam` | no step at the loop | -34.1 dB `max_click` | the same loop without the seam fade reads -0.0 dB; the 100 Hz sine's own slope floor is -37.7 dB |
| `transport-drift` | follows the transport | 0.000 ms over 60 s | 60 impulses, worst deviation under one sample |
| `kweighting-rates` | +/-0.1 LU at every rate | 0.020 LU | EBU 3341 case 1 at 44.1/48/88.2/96/176.4/192 kHz reads -22.99 to -23.02 LUFS |

The Integrated match error is bounded by the 0.1 LU hysteresis by construction (brief 4.2:
the gain only moves when the reading moves by more than 0.1 LU), so 0.070 LU is the
hysteresis band, not noise. Short 3 s has no hysteresis and reads 0.030 LU.

## Rule 13: every presence test shown failing on the build with its defect (D.3.8)
Two scratch builds in the working tree (restored afterwards; `git status` clean, `ctest
--preset all` 5/5 again). Each defect is one line; the tests are grouped so that no defect in
a build can explain another's failure.

| Build | Planted defect | Test that failed | What it said |
|---|---|---|---|
| A | `wireOnly = false` (render safety off) | `test_render_is_always_the_wire` | "the plugin did not notice the offline render" |
| A | `FadeLaw::equalPower` on the switch | `test_switch_does_not_overshoot` | "the switch overshot by +3.0086 dB" - the same number the prototype measured at G3 |
| A | `kSeamFadeSeconds = 0.0` | `test_loop_seam_is_not_a_step` | "the loop seam reads -0.0 dB" |
| B | `readout()` returns a bare number | `test_match_readout_says_what_it_did` | "the readout must name the side and carry a sign: '6.0'" |
| B | `trimSmoothed.reset(trim + 0.001f)` (a gain sneaks into the wire) | `test_mix_mode_is_bit_exact_wire` | "Mix mode is not the wire: residual -103.4 dBFS" |
| B | `player.setAlign(0, 0)` | `test_align_pair_shifts_the_reference` | "0.0 != 0.5 within 0.002 delta" |
| B | no eviction in `ReferenceStore::setActive` | `test_at_most_two_references_are_resident` | "3 not less than or equal to 2" |
| B | the blind draw constant | `test_blind_assignment_is_seeded` | "Items in the second set but not the first: True" |

`test_plugin_matches_the_prototype` also failed in build B, as it must: the trim offset is a
real difference from the prototype. The remaining tests are contract and health checks, which
guard defects the harness itself reports (NaN, latency, state) rather than a feature.

## Listening debt
- 2026-11-01: nothing heard yet in this session (no audio device, no macOS). Every render is
  on disk under `sound design/renders/` for the listening checkpoint above.

## Open exceptions
- none yet.

## Handoff
Task: build A B see from G1 to a validated Tier P plugin, shared modules aside.
Plan: 1. [x] G1 decisions 2. [x] G2 numpy prototype 3. [x] G3 measure the prototype (10/10)
4. [x] G4 port to C++ with the seams and the harness hooks 5. [x] test suite (fast 16 tests
/ 50 s, render 15 tests / 70 s) 6. [x] preset bank (9, lint green) 7. [x] G5 null against the
prototype 8. [x] one committed golden 9. [x] every presence test proven on a planted-defect
build (table above) 10. [ ] **paused here 2026-11-01 at Björn's request; the work continues in
a Visual Studio Code session on his Mac: `docs/continue.md` is the prompt to paste** 11. [ ] the
editor, when `Source/oberli/` lands (plugin-ui-bob) 12. [ ] README, CHANGELOG, notices
(ship-bob) 13. [ ] on the Mac: validators, malloc trap / RTSan, the perf row, the two hosts.
Working tree: clean. Build: green (Linux/x86_64, GCC 13 - **not** a slice that ships).
Tests: `ctest --preset all` -> 5/5 (fast 50 s, render 70 s, three lints).
Open questions for the owner: the Waiting-for-Björn lines above.
Next command: `cmake --preset release && cmake --build --preset release && ctest --preset all`

The prototype's renders are **not** committed: `.gitignore` keeps WAVs out of this repo
(project policy, CLAUDE.md) and the renders are reproducible in one seeded command
(`absee_proto.py --demo`, rule 8). The committed G2/G3 artefacts are the prototype, the
measurement script and `g3-prototype.json`.

## Decisions
- 2026-11-01: **G1 family.** A B see is track A: the product is two measurements and a
  switch, not a processor. Per `dsp-selection` S.4 the loudness meter is BS.1770-5 with
  K-weighting **redesigned per rate** (reusing the 48 kHz table reads 0.68 dB low on pink
  noise at 96 kHz, A.9), gated integrated, 3 s short-term, and a 4x true-peak detector; that
  meter is the shared `oberli::Loudness`, not written here. A B see's own DSP is: (1) a 5 ms
  equal-power (sin/cos) crossfade between the mix and the reference, started at a block
  boundary; (2) a match-gain policy on top of the meter's readings - difference of the two
  readings, 0.1 LU hysteresis in Integrated mode so a wandering measurement cannot pump, one
  20 ms one-pole smoother in the dB domain; (3) a reference player reading a
  RAM buffer at session rate with a section loop and a 5 ms seam crossfade; (4) the transport
  map; (5) render safety. Per S.5: the reference buffers are large objects, so they are built
  off the audio thread and published by atomic pointer swap with off-thread reclamation
  (pattern B, D.1.4) - never decoded, resampled or freed in `processBlock`; parameters reach
  the audio thread as cached APVTS atomics; the editor is fed by atomics and an SPSC FIFO.
  **Trade-off:** the whole design is chosen for *transparency over cleverness*. The mix path
  is a wire with no gain stage in Mix mode at Trim 0 (bit-exact null, the brief's `thd_n`
  target), the reference is never resampled per block (resampling happens on load, so a
  rate mismatch makes a slot silent rather than audibly interpolated), and the only
  nonlinearity in the product is none at all - so rule 3 (aliasing) and rule 4 (feedback)
  have nothing to bite on, and the risk moves entirely to the two places where this kind of
  plugin actually fails: a bounce that is not the mix, and a measurement that lies. Both get
  a named test (`test_render_is_always_the_wire`, `test_match_readout_says_what_it_did`).
- 2026-11-01: **Departure 1 from rule 2: a 5 ms switch crossfade, not 10-20 ms.** The brief
  is owner-confirmed on 5 ms and the hand wants the switch to feel instant. Equal-power
  sin/cos over 5 ms has no discontinuity, so the risk is a level dip or an audible seam, not
  a click. Rejected if the brief's own test fails: 1 kHz sine on both sides, 200 switches, any
  sample above peak + 0.01 dB (`proto_measure.py --case switch-click`), or if the 5 ms fade
  reads worse than a 10 ms fade by more than 0.5 dB in `max_click_db`. Raised with the owner
  (Waiting for Björn).
- 2026-11-01: **Departure 2 from the sketch's wording: the transport map is in seconds, not
  bar position** (sketch 4.3 says "maps the DAW's bar position"). The reference is a fixed
  audio file, so one DAW second must map to one reference second; a ppq map would slide the
  reference against the music on every tempo change and would need the host's tempo to be
  stable, which it is not in a worship set. The align gesture is still made at a bar: it
  stores the pair (DAW seconds, reference seconds) at the moment of the press. Rejected if
  following the transport drifts more than 1 ms over 60 s at 120 bpm
  (`proto_measure.py --case transport-drift`). Not a structural decision of the sketch
  (section 4.3, not section 1), so it is decided here and recorded.
- 2026-11-01: **Departure 3 from the brief's letter: the switch fades equal-gain, not
  equal-power.** The brief's `must_do` says "5 ms equal-power crossfade"; its own acceptance
  test says "1 kHz sine both sides, 200 switches, no sample above peak + 0.01 dB". The two
  cannot both hold: with the same signal on both sides `sin + cos` peaks at **+3.0086 dB**
  (measured, `proto_measure.py --case switch-click`), so an equal-power switch between two
  masters at -1 dBTP clips the monitoring path. Equal-gain (`a + b = 1`) measures +0.0000 dB
  on that test and costs a 3.01 dB dip at the midpoint of the fade when the two sides are
  uncorrelated - under half a millisecond, and the dip is the reason the fade is there.
  The acceptance test wins over the word in `must_do`. Rejected if a level-matched blind
  A/B of the two laws is audibly different at the switch (the listening checkpoint), or if
  the uncorrelated dip measures worse than -3.1 dB. Raised with the owner.
- 2026-11-01: **The section-loop seam keeps the equal-power law**, because it joins two
  *uncorrelated* points of the same file (the section's end and its start), where equal-power
  is the law that keeps the level. One fade length (5 ms), two laws, each measured.
- 2026-11-01: **Constant: the 5 ms section-loop seam crossfade.** A section loop jumps from
  `out` back to `in`; without a fade that step is a click on any material. The same 5 ms
  equal-power fade as the switch is used (one constant, one measurement), reading the buffer
  twice. Rejected by `proto_measure.py --case loop-seam` above -40 dB `max_click_db`.
- 2026-11-01: **`refSlot` is a `choice` of eight, not an `int` 1-8** (the sketch's table said
  int). `juce::AudioParameterInt` does not override `isDiscrete()`, and JUCE's VST3 wrapper
  reports `stepCount = 0` for a parameter that is not discrete, so Ableton would have drawn
  the reference slot as a continuous knob instead of eight positions. Found by
  `paramgen.py check --list-params` ("refSlot: harness steps 0 != table 8"), which is exactly
  what that check is for. The user sees the same thing ("1".."8"); the raw value is now the
  0-based index, which the processor, the harness and the nine presets follow.
- 2026-11-01: **Build fix:** `add_dependencies(ABSeeHarness ABSee)` in CMakeLists.txt. The
  harness includes `ABSee_artefacts/JuceLibraryCode` for `JucePluginDefines.h`, which juceaide
  only generates as part of the `ABSee` target; building the harness alone failed with
  `'JucePlugin_Name' was not declared` (log:
  `build1.log`, 2026-11-01). Without the dependency the CI job's parallel build is a race.
