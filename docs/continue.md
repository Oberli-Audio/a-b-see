# A B see: continue on the Mac (Visual Studio Code session)

The cloud session stopped on 2026-11-01 after G5, with the branch
`claude/a-b-see-oberli-audio-0ycavk` pushed. Everything it decided, measured and left open is
in `docs/status.md`; this file is only the prompt to start the next session with, plus the few
things that session needs to know and cannot read off the repo.

## Prompt to paste

```
Continue A B see for Oberli Audio, Tier P. Read docs/brief.md, docs/status.md and
docs/failures.md first; status.md carries the gates, the decisions, the three
departures, the Waiting-for-Björn list and the rule-13 proof table. Use
sound-design-bob and plugin-ui-bob (ship-bob for CI and releases). State tier,
track and gate in your first line.

Where it stands: G0-G4 closed, G5 part-closed, G6 part-closed. The DSP is ported
and the suite is green on Linux/x86_64 (ctest --preset all -> 5/5). What is still
missing needs this machine or the shared modules:

1. The release-gate work that needs macOS, in this order:
   cmake --preset release && cmake --build --preset release && ctest --preset all
   PRODUCT_MALLOC_TRAP_SELFTEST=1 ./build/ABSeeHarness_artefacts/Release/ABSeeHarness
     --signal=impulse        # must abort; then run the suite with the trap live (rule 1)
   eval "$(python3 scripts/dsp-kit/toolchain.py env)" && cmake --preset rtsan &&
     cmake --build --preset rtsan
   python3 scripts/dsp-kit/perf.py run --harness <harness> --arch x86_64   # rule 20, 3 runs
   python3 scripts/dsp-kit/validate_plugin.py --profile full --plugin
     "build/ABSee_artefacts/Release/VST3/A B see.vst3"
   Regenerate the golden on the x86_64 slice (tools/tests/make_golden.py) and say so
   in docs/status.md: the committed one came from Linux/GCC, so it is a
   cross-slice golden until then.

2. Working rules, unchanged: pass a gate on your own when its evidence is green and
   log it in docs/status.md with the command behind every number; stop for Björn only
   for listening checkpoints, the two Ableton host checks, a conflict with a
   structural decision in the sketch, and before any release (run the release gate).
   Everything that needs him goes in "Waiting for Björn" at the top of
   docs/status.md, one line each, newest first. Keep working on whatever is not
   blocked. Commit and push at every checkpoint, to
   claude/a-b-see-oberli-audio-0ycavk.

3. Do not write the shared code. LookAndFeel, Strip frame, meters, Loudness,
   Analysis/FFT, References and RenderSafety are built in Oberli-Audio/bob-skills as
   template Source/oberli/ and must never be reimplemented here or edited there.
   A B see needs Loudness and References; until they land the editor stays the
   template stub and the match gain stays 0.0 dB. The seams they plug into are
   Source/MixAnalysis.h (the meter's contract, with NoMixAnalyser as the stand-in)
   and Source/ReferenceStore.h (what References publishes). When they land: refresh
   Source/oberli/ from plugin-template, wire them in, then the tests marked in
   status.md become real (the match against a live meter, the seven band deltas) and
   test_band_deltas_are_not_measurable_yet must be replaced by the real presence test.

4. The editor is plugin-ui-bob's job and has not started. Accent is peacock #039BE5
   (category accent for analysis and monitoring, CLAUDE.md), theme Bone; the sketch
   docs/sketch.md has the layout, the states and the five core controls.
```

## What the next session cannot read off the repo

- **The build slice.** Every number in `docs/status.md` with a `build.arch` of `x86_64` came
  from Linux with GCC 13 in a cloud container: no macOS, no Faust, no pluginval, no RTSan, no
  Rosetta, no audio device. Nothing in the repo has been *heard* yet, and the committed golden
  `tools/tests/golden/switch-and-match.wav` was rendered by that compiler. Treat it as a
  cross-slice golden (tolerance -100 dBFS, `GOLDEN_PEAK_DB["cross-feedforward"]`) until it is
  re-rendered on the Mac.
- **The prototype's renders are not in git** (`.gitignore` keeps WAVs out): one seeded command
  brings them back, `python3 "sound design/prototype/absee_proto.py" --demo`, and they are what
  the listening checkpoint listens to.
- **The harness has A B see's own flags** (`--reference`, `--marks`, `--align`, `--transport`,
  `--mix-reading`, `--mix-meter=prepass`, `--non-realtime`, `--safe-on-render`), documented in
  `DEVELOPING.md` and in `--help`. Each one makes exactly the call the missing shared module
  will make, which is why the tests survive the swap.
- **Three defects were found and fixed by the checks themselves**, and two template bugs were
  fixed in this repo that `Oberli-Audio/plugin-template` still has. Both are in `docs/status.md`.
