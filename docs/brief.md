# Sonic brief (owner-confirmed 2026-11-01 from the decided Tier S sketch, docs/sketch.md; amend below, never edit the YAML)

```yaml
name: A B see
track: analysis
stages: [processing, output, analysis]
type: utility
musical_role: effect
tier: P
one_sentence: Switches the master between your mix and a reference track at matched loudness, click-free, and shows per band how far apart they are; a bounce is always the untouched mix.
references:
  - ADPTR Metric AB: same job; A B see is free, Ableton-only, with blind X/Y and per-reference sections
  - Mastering The Mix REFERENCE 3: same job; A B see adds true-peak matching and a forced wire on render
  - Melda MCompare: compares plugin chains, not reference files
must_do:
  - Loudness-match the reference to the mix within 0.1 LU (Integrated, Short 3 s) or 0.1 dB (True peak)
  - Switch Mix/Reference with a 5 ms equal-power crossfade, no click
  - Render (isNonRealtime) is always the wire, whatever state the plugin was left in
  - Up to 8 references with 4 sections each plus Loudest 10 s, looped, following the transport or free-running
  - Blind X/Y with a tally
defining_features:
  - Bounce null: a bounce left in Reference mode, Blind on, Trim +6 dB nulls against the plugin removed
  - Match readout states what was done, to which side, in dB ("Reference -1.4 dB")
  - Seven band deltas (mix minus reference, level matched) as the hero
out_of_scope: [EQ matching or "make it like the reference", streaming pass/fail presets, cloud reference library]
core_controls:
  - Listen: Mix or Reference at matched loudness
  - Reference: which of 8 slots plays
  - Match: Integrated, Short 3 s, True peak, Off
  - Blind: hides which side is which, keeps a tally
  - Trim: monitoring level of both sides, forced to 0 dB on render
signal_flow: |
  in --+----------------------- (wire) ---------------+
       +-- meter (LUFS I/S, TP, PSR, spectrum, width) |-- crossfade 5 ms -- Trim -- out
  reference RAM -- section loop / transport map -- match gain -- meter --+
cpu_budget: { p999_pct: 25, max_pct: 50, idle_pct: 2, at: 48k/64, slice: x86_64-rosetta, preset: Init with reference playing and editor open }
cpu_reference_machine: Apple M3 Pro (Björn's Mac)
instances_expected: 1 per project, last device on the master
latency_budget: 0 samples in every mode
sample_rates: [44100, 48000, 88200, 96000, 176400, 192000]
channels: stereo
register: n/a (effect)
register_intent: n/a
presets:
  bank_size: 8
  categories: [Mastering, Mixing, Live]
  reference_chord: n/a; test material is the harness's pink noise and the bundled test songs
  loudness_window: n/a (Trim 0 dB in every preset)
  edge_presets: [True Peak Compare, Short-Term Verse, Blind Session]
  naming: English, by use ("Master vs Reference", "Verse Check Short-Term")
mix_context: last on the Ableton master after the limiter; never colours the mix; the reference never reaches a bounce
quality_targets:
  alias_floor_db:     { target: n/a, method: n/a (no nonlinear processing) }
  thd_n_db:           { target: wire, method: bit-exact null of the mix path in Mix mode }
  response_error_db:  { target: 0.1 LU match, method: harness renders the mix as its own reference at -6 dB; readout must say Reference +6.0 dB }
  loudness:           { target: BS.1770-4 within 0.1 LU of a reference meter, method: EBU Tech 3341 test signals through the harness }
  keyboard_span_lu:   { target: n/a, method: n/a }
  low_register:       { target: n/a, method: n/a }
  other:
    - "switch click: 1 kHz sine both sides, 200 switches, no sample above peak + 0.01 dB"
    - "render null: harness offline render in every state equals the input"
    - "memory: 8 slots loaded, at most 2 decoded buffers resident"
open_questions: []
```

## Amendments
- 2026-11-01: Owner decisions confirmed with the brief: "Trim mix instead" defaults to off; section markers are per reference; references live in the shared Oberli Audio/References/ folder with Bench Line; Loudness and References come from the shared Source/oberli/ code (bob-skills), not written here.
