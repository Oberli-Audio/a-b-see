# CPU - A B see (rule 20)

Written by `perf.py run` on 2026-11-02, commit 8096712, release preset, both slices of the universal harness (x86_64 under Rosetta: the Live 10.1 slice). Percent of the 64-sample deadline at 48 kHz. Budget: p99.9 < 25 %, recurring max < 50 %. `perf.py` loads no reference, so these cover the always-running meter path; reference playback adds a copy and two gains per sample. Baseline: `docs/perf.json`.

```bash
python3 scripts/dsp-kit/perf.py run --harness build/ABSeeHarness_artefacts/Release/ABSeeHarness --arch x86_64 --presets all --config 48000/64 --runs 3 --baseline docs/perf.json
python3 scripts/dsp-kit/perf.py run --harness build/ABSeeHarness_artefacts/Release/ABSeeHarness --presets all --config 48000/64 --runs 3 --baseline docs/perf.json
```

| arch | preset | fs/block | p50 | p99 | p99.9 | max | reading | first block | over (reported) |
|---|---|---|---|---|---|---|---|---|---|
| x86_64 | init | 48000/64 | 0.3 | 1.5 | 1.6 | 3.7 at 58.87 s | scheduling | 2 | 0 |
| x86_64 | master-vs-reference | 48000/64 | 0.2 | 1.5 | 1.6 | 3.0 at 29.50 s | scheduling | 2 | 0 |
| x86_64 | true-peak-compare | 48000/64 | 0.2 | 1.5 | 1.6 | 2.2 at 29.10 s | scheduling | 2 | 0 |
| x86_64 | loud-master-meet | 48000/64 | 0.3 | 1.5 | 1.6 | 22.0 at 0.82 s | scheduling | 2 | 0 |
| x86_64 | raw-levels | 48000/64 | 0.3 | 1.5 | 1.6 | 9.2 at 45.05 s | scheduling | 2 | 0 |
| x86_64 | verse-check-short | 48000/64 | 0.3 | 1.5 | 1.9 | 5498.1 at 46.25 s | scheduling | 2 | 11 |
| x86_64 | blind-session | 48000/64 | 0.3 | 1.6 | 2.1 | 891.2 at 7.98 s | scheduling | 4 | 20 |
| x86_64 | chorus-vs-chorus | 48000/64 | 0.2 | 1.5 | 1.6 | 5.7 at 46.90 s | scheduling | 2 | 0 |
| x86_64 | live-room-check | 48000/64 | 0.2 | 1.5 | 1.6 | 2.3 at 4.50 s | scheduling | 2 | 0 |
| arm64 | idle | 48000/64 | 0.2 | 1.4 | - | - | idle | - | - |

| arch | preset | fs/block | p50 | p99 | p99.9 | max | reading | first block | over (reported) |
|---|---|---|---|---|---|---|---|---|---|
| arm64 | init | 48000/64 | 0.2 | 1.4 | 1.5 | 2.0 at 52.60 s | scheduling | 1 | 0 |
| arm64 | master-vs-reference | 48000/64 | 0.2 | 1.4 | 1.5 | 68.1 at 57.87 s | scheduling | 0 | 0 |
| arm64 | true-peak-compare | 48000/64 | 0.2 | 1.4 | 1.5 | 4.6 at 27.68 s | scheduling | 0 | 0 |
| arm64 | loud-master-meet | 48000/64 | 0.2 | 1.4 | 1.5 | 5.5 at 21.33 s | scheduling | 0 | 0 |
| arm64 | raw-levels | 48000/64 | 0.2 | 1.4 | 1.7 | 32.7 at 44.87 s | scheduling | 1 | 0 |
| arm64 | verse-check-short | 48000/64 | 0.2 | 1.5 | 1.6 | 2.2 at 31.45 s | scheduling | 0 | 0 |
| arm64 | blind-session | 48000/64 | 0.2 | 1.5 | 1.6 | 2.1 at 4.40 s | scheduling | 1 | 0 |
| arm64 | chorus-vs-chorus | 48000/64 | 0.2 | 1.5 | 1.6 | 2.3 at 19.80 s | scheduling | 1 | 0 |
| arm64 | live-room-check | 48000/64 | 0.2 | 1.5 | 1.6 | 14.6 at 40.70 s | scheduling | 0 | 0 |
| arm64 | idle | 48000/64 | 0.2 | 1.5 | - | - | idle | - | - |
