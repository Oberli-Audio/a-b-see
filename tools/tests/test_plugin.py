#!/usr/bin/env python3
"""A B see - the test suite (sound-design-bob D.3, from the kit's test_template.py).

Drives the real plugin through `ABSeeHarness` (harness-contract.md v3 plus A B see's own
flags, `--help`) and asserts on its JSON and WAVs with `dspkit`.

    HARNESS=build/ABSeeHarness_artefacts/Release/ABSeeHarness python3 tools/tests/test_plugin.py --label fast
    ctest --preset fast

Labels (D.3.1): `fast` = contract, the rate/block matrix, health, state (every build, under
60 s); `render` = the defining features, the brief's acceptance tests, the goldens and the
null test against the prototype. The host validators are not here (validate_plugin.py).

Every test names the rule or the brief line it guards. Each was shown failing on a build with
the defect it guards (rule 13, D.3.8); the two scratch builds and which tests failed on each
are in docs/status.md under G4.

What is **not** tested here, because the shared modules have not landed (docs/status.md,
Waiting for Björn): the loudness match against a real streaming meter (the harness injects a
constant reading, so what is tested is the match policy), and the seven band deltas, which
need Analysis/FFT.
"""
import json
import math
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, os.environ.get("DSPKIT", str(ROOT / "scripts" / "dsp-kit")))
sys.path.insert(0, str(ROOT / "sound design" / "prototype"))
import dspkit as K  # noqa: E402
import absee_proto as P  # noqa: E402

HARNESS = os.environ.get("HARNESS", "build/ABSeeHarness_artefacts/Release/ABSeeHarness")
GOLDEN_DIR = Path(os.environ.get("GOLDEN_DIR", "tools/tests/golden"))
GOLDEN_ARCH = os.environ.get("GOLDEN_ARCH", "x86_64")
DEFAULT = dict(seed=1, sample_rate=48000, block=512, seconds=2.0)
SOURCE = dict(signal="noise:pink:-20")     # a mix, not an impulse: this plugin's input is programme
PARAM = ("trim", 0.75)                     # +12 dB, a real value that is not the default
RATES = (44100, 48000, 88200, 96000, 176400, 192000)
BLOCKS = (0, 1, 13, 32, 64, 512, 4096)
FAST_BUDGET_S = float(os.environ.get("FAST_BUDGET_S", "20"))
# F.8 tolerances (peak of the residual, absolute dBFS, no alignment)
GOLDEN_PEAK_DB = {"same": -140.0, "cross-feedforward": -100.0, "cross-feedback": -80.0}

# parameters.csv: a choice parameter's normalised value is index / (states - 1)
LISTEN = {"mix": 0.0, "reference": 1.0}
MATCH = {"integrated": 0.0, "short": 1.0 / 3.0, "truepeak": 2.0 / 3.0, "off": 1.0}
SECTION = {"whole": 0.0, "1": 0.2, "2": 0.4, "3": 0.6, "4": 0.8, "loudest": 1.0}
TRIM0 = 0.5                                 # -24..+24 dB, so 0 dB is the middle
REF_SECONDS = 4.0      # the test references; long enough for a section loop, short enough that
                       # analysing one (six sections, 4x true peak) stays inside the fast budget


def db_to_gain(d):
    return 10.0 ** (d / 20.0)


def run(params=None, **flags):
    f = dict(DEFAULT, **SOURCE)
    f.update(flags)
    res = K.render(HARNESS, params, **f)
    echo = {k: v for k, v in f.items() if k in ("seed", "sample_rate", "block", "seconds", "preset")
            and not (k == "block" and str(v).startswith("random:"))}
    K.assert_echo(res, params, **echo)                                     # rule 13
    return res


class Material:
    """The test references, written once per run: seeded pink noise, so every render is
    reproducible (rule 8). Real music lives in OneDrive and never in a test."""

    dir = None

    @classmethod
    def setup(cls):
        if cls.dir is not None:
            return
        cls.dir = Path(tempfile.mkdtemp(prefix="absee-refs-"))
        fs = 48000
        n = int(REF_SECONDS * fs)
        cls.quiet = cls.dir / "quiet.wav"          # 6 dB below the harness's pink mix
        K.write_wav(cls.quiet, P.pink(n, fs, -26.0, seed=2).astype(np.float32), fs)
        cls.quiet44 = cls.dir / "quiet44.wav"      # the same, at 44.1 kHz: resampled on load
        K.write_wav(cls.quiet44, P.pink(int(REF_SECONDS * 44100), 44100, -26.0, seed=2).astype(np.float32), 44100)
        cls.other = cls.dir / "other.wav"
        K.write_wav(cls.other, P.pink(n, fs, -26.0, seed=3).astype(np.float32), fs)
        cls.third = cls.dir / "third.wav"
        K.write_wav(cls.third, P.pink(n, fs, -26.0, seed=4).astype(np.float32), fs)
        cls.sine = cls.dir / "sine1k.wav"          # the brief's switch test material
        K.write_wav(cls.sine, P.sine(n, fs, 1000.0, -12.0).astype(np.float32), fs)
        cls.low = cls.dir / "sine100.wav"          # the seam test: a sine whose own slope is low
        K.write_wav(cls.low, P.sine(n, fs, 100.0, -12.0).astype(np.float32), fs)
        cls.ticks = cls.dir / "ticks.wav"          # one impulse per second: the transport map
        ticks = np.zeros((n, 2), dtype=np.float32)
        for s in range(int(REF_SECONDS)):
            ticks[s * fs] = 1.0
        K.write_wav(cls.ticks, ticks, fs)


class Timed(unittest.TestCase):
    label = "fast"

    @classmethod
    def setUpClass(cls):
        Material.setup()

    def setUp(self):
        self._t0 = time.time()

    def tearDown(self):
        dt = time.time() - self._t0
        print("  [%s %.1f s]" % (self.label, dt), end="", flush=True)
        if self.label == "fast" and dt > FAST_BUDGET_S:
            self.fail("fast test took %.0f s; budget %.0f s (D.3.6: move it to a slower tier, do not delete it)" % (dt, FAST_BUDGET_S))


#================================================================= fast: the contract
class Contract(Timed):
    def test_harness_echoes_params(self):                                  # rule 13
        pid, val = PARAM
        res = run({pid: val})
        self.assertAlmostEqual(res["applied"]["params"][pid], val, places=6)

    def test_seeded_runs_are_bit_identical(self):                          # rule 8
        params = {"listen": LISTEN["reference"], "matchMode": MATCH["off"], "followTransport": 0.0}
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.wav", Path(d) / "b.wav"
            run(params, reference=str(Material.quiet), out=a)
            run(params, reference=str(Material.quiet), out=b)
            xa, _ = K.read_wav(a)
            xb, _ = K.read_wav(b)
            self.assertTrue((xa == xb).all(), "two seeded renders differ")

    def test_latency_is_zero_and_matches_impulse(self):                    # rule 7, brief: 0 samples
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "imp.wav"
            res = run(signal="impulse", out=out)
            self.assertEqual(res["applied"]["latency_samples"], 0)
            x, fs = K.read_wav(out)
            ref = x[:, :1] * 0.0
            ref[0, 0] = 1.0
            self.assertEqual(K.latency_samples(ref, x[:, :1]), 0)


class RateAndBlock(Timed):
    def test_rate_block_matrix_is_clean(self):                             # DoD: 42 combinations
        for fs in RATES:
            for blk in BLOCKS:
                with self.subTest(fs=fs, block=blk):
                    # A render made only of zero-length blocks never progresses, so the DoD's
                    # "block 0" cell is rendered as random blocks drawn from {0, 1}: every
                    # block is 0 or 1 sample, which hammers the zero case (D.5.1).
                    flags = dict(block="random:1", max_block=1) if blk == 0 else dict(block=blk)
                    res = run(sample_rate=fs, seconds=0.25, **flags)
                    self.assertEqual(res["health"]["nan"] + res["health"]["inf"], 0)
                    self.assertLess(res["levels"]["peak_dbfs"], 0.0)

    def test_random_block_sizes_from_zero(self):                           # D.5.1, fuzz
        res = run(block="random:7", max_block=1024, seconds=0.5)
        self.assertEqual(res["health"]["nan"], 0)

    def test_block_larger_than_announced(self):                            # D.5.1
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "ref.wav", Path(d) / "big.wav"
            run(block=256, max_block=256, seconds=0.5, out=a)
            res = run(block=1024, max_block=256, seconds=0.5, out=b)
            self.assertEqual(res["health"]["nan"], 0)
            xa, fs = K.read_wav(a)
            xb, _ = K.read_wav(b)
            self.assertLess(K.dropout_db(xa, xb, fs), 3.0,
                            "an oversized block rendered quieter than its announced-size twin")

    def test_reference_at_the_wrong_rate_is_silent_not_interpolated(self):  # ReferencePlayer.h
        # oberli::References resamples on load; a buffer at another rate must not be read at
        # all - a wrong-pitch reference that sounds plausible is worse than silence in a tool
        # whose job is to be trusted. The case is the host changing rate while loaded: the
        # reference is published at 48 k and the session moves to 96 k mid-render.
        # K.render, not run(): after a re-prepare the harness echoes the rate it ended on
        # (96000), which is right, so the rule-13 echo check cannot compare it with the rate
        # the render started at.
        res = K.render(HARNESS, {"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                                 "followTransport": 0.0},
                       seed=1, sample_rate=48000, block=512, seconds=1.0,
                       signal="noise:pink:-20", reference=str(Material.quiet),
                       reprepare_at="0.5:96000/512")
        self.assertEqual(res["applied"]["sample_rate"], 96000)
        self.assertTrue(res["product"]["reference"]["rate_mismatch"])
        self.assertFalse(res["product"]["reference"]["playing"])
        self.assertEqual(res["health"]["nan"] + res["health"]["inf"], 0)


    def test_reference_at_another_rate_is_resampled_on_load(self):  # oberli::references::load
        """A 44.1 kHz file in a 48 kHz session plays, at its own loudness: References resamples
        on load (windowed sinc), so the player never sees a rate mismatch."""
        res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"], "section": SECTION["whole"],
                   "followTransport": 0.0}, reference=str(Material.quiet44), seconds=REF_SECONDS)
        self.assertTrue(res["product"]["reference"]["playing"])
        self.assertFalse(res["product"]["reference"]["rate_mismatch"])
        source, fs = K.read_wav(Material.quiet44)
        self.assertAlmostEqual(res["loudness"]["integrated_lufs"], K.integrated_lufs(source, fs), delta=0.1)


class Health(Timed):
    def test_silence_stays_silent(self):                                   # DoD: silence test
        # Mix mode on a silent input, with a reference loaded and looping behind the switch:
        # the output must be silent, with no DC and no denormal storm. The reference player
        # runs whichever side the switch is on, so that switching back is in time.
        res = run({"listen": LISTEN["mix"], "followTransport": 0.0, "matchMode": MATCH["integrated"]},
                  signal="silence", seconds=2.0, sample_rate=48000, block=64,
                  reference=str(Material.quiet), mix_meter="prepass")
        self.assertLess(res["levels"]["peak_dbfs"], -200.0, "a silent input did not stay silent")
        self.assertLess(res["levels"]["dc_dbfs"], -80.0)
        self.assertEqual(res["health"]["nan"] + res["health"]["inf"], 0)

    def test_mix_mode_is_bit_exact_wire(self):                             # brief: thd_n = wire
        # The mix path has no gain stage: Mix mode at Trim 0 must be the input, bit for bit,
        # and identical to the plugin bypassed.
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "mix.wav", Path(d) / "bypass.wav"
            run({"listen": LISTEN["mix"], "trim": TRIM0, "matchMode": MATCH["integrated"]},
                reference=str(Material.quiet), mix_meter="prepass", out=a)
            run({"bypass": 1.0}, out=b)
            xa, _ = K.read_wav(a)
            xb, _ = K.read_wav(b)
            self.assertTrue((xa == xb).all(),
                            "Mix mode is not the wire: residual %.1f dBFS"
                            % K.null_residual(xb, xa, align=False)["peak_dbfs"])

    def test_bypass_nulls_against_the_input(self):                         # rule 7
        # An impulse is the one input this test can know exactly without asking the plugin,
        # so the null is against the input itself, not against another render.
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "bypassed.wav"
            res = run({"bypass": 1.0, "listen": LISTEN["reference"], "trim": 0.9, "mono": 1.0},
                      reference=str(Material.quiet), signal="impulse", out=out, seconds=1.0)
            x, fs = K.read_wav(out)
            expected = np.zeros_like(x)
            expected[0, :] = 1.0
            self.assertTrue((x == expected).all(),
                            "bypass is not the input: peak %.1f dBFS after the impulse"
                            % K.peak_dbfs(x[1:]))
            self.assertEqual(res["applied"]["latency_samples"], 0)

    def test_mono_sums_at_minus_three_db(self):                            # sketch 4.1
        with tempfile.TemporaryDirectory() as d:
            mono, stereo = Path(d) / "mono.wav", Path(d) / "stereo.wav"
            run({"mono": 1.0}, out=mono, seconds=1.0)
            run({"mono": 0.0}, out=stereo, seconds=1.0)
            m, fs = K.read_wav(mono)
            s, _ = K.read_wav(stereo)
            self.assertTrue(np.allclose(m[:, 0], m[:, 1], atol=1e-7), "mono output is not mono")
            # the harness's pink noise is the same in both channels, so L+R at -3 dB is +3 dB
            measured = K.rms_dbfs(m) - K.rms_dbfs(s)
            self.assertAlmostEqual(measured, 3.0103, delta=0.05)


class Properties(Timed):
    def test_state_round_trip_is_bytewise(self):                           # D.5.3
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            pid, val = PARAM
            run({pid: val}, state_out=d / "a.state", out=d / "a.wav", seconds=0.5,
                marks="1,1,2.0,3.5", align="1,1.5,4.25", reference=str(Material.quiet))
            rb = run(state_in=d / "a.state", state_out=d / "b.state", out=d / "b.wav", seconds=0.5)
            self.assertEqual((d / "a.state").read_bytes(), (d / "b.state").read_bytes())
            self.assertAlmostEqual(rb["applied"]["params"][pid], val, places=6)
            xa, _ = K.read_wav(d / "a.wav")
            xb, _ = K.read_wav(d / "b.wav")
            self.assertTrue((xa == xb).all())

    def test_state_carries_marks_and_align(self):                          # sketch 3, State.h Extra
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            run(state_out=d / "s.state", seconds=0.25, reference=str(Material.quiet),
                marks="1,1,2.0,3.5", align="1,1.5,4.25")
            text = (d / "s.state").read_bytes()
            # the state is a compressed XML blob; reload it and read the section back
            res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                       "followTransport": 0.0, "section": SECTION["1"]},
                      state_in=d / "s.state", reference=str(Material.quiet), seconds=0.5)
            self.assertAlmostEqual(res["product"]["reference"]["section_in_s"], 2.0, places=3)
            self.assertAlmostEqual(res["product"]["reference"]["section_out_s"], 3.5, places=3)
            self.assertGreater(len(text), 0)

    def test_init_preset_equals_the_table_defaults(self):                  # F.14.3
        # presetlint's own version of this check compares a choice parameter's plain default
        # with the harness's normalised echo, so it fails for any choice default but index 0
        # (docs/status.md, Waiting for Björn). Here both sides are normalised through the
        # kit's own `to_normalised`, which is what the harness echoes.
        sys.path.insert(0, str(ROOT / "scripts" / "dsp-kit"))
        import paramgen as PG
        table = PG.read_table(str(ROOT / "parameters.csv"))
        res = run(preset="init", seconds=0.25)
        applied = res["applied"]["params"]
        for p in table:
            with self.subTest(param=p["id"]):
                self.assertAlmostEqual(applied[p["id"]], PG.to_normalised(p, p["fdefault"]),
                                       places=4, msg="Init must equal the parameter table")

    def test_every_preset_loads_and_is_healthy(self):                      # D.6.5
        for row in K.list_presets(HARNESS):
            with self.subTest(preset=row["slug"]):
                res = run(preset=row["slug"], seconds=0.5)
                self.assertEqual(res["health"]["nan"] + res["health"]["inf"], 0)
                self.assertLess(res["levels"]["true_peak_dbtp"], -1.0)

    def test_corrupted_state_does_not_crash(self):                         # D.5.3, fuzz
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            run(state_out=d / "good.state", seconds=0.25)
            good = (d / "good.state").read_bytes()
            cases = {"empty": b"", "truncated": good[: len(good) // 2],
                     "garbage": bytes(range(256)) * 4, "flipped": bytes(b ^ 0x55 for b in good)}
            for name, blob in cases.items():
                with self.subTest(case=name):
                    (d / "bad.state").write_bytes(blob)
                    try:
                        res = K.render(HARNESS, state_in=d / "bad.state", seconds=0.25,
                                       **{k: v for k, v in DEFAULT.items() if k != "seconds"}, **SOURCE)
                    except RuntimeError as e:
                        self.assertNotIn("Segmentation", str(e))
                        self.assertNotIn("-11", str(e).split(")")[0])
                        continue
                    self.assertEqual(res["health"]["nan"], 0)


#================================================================= render: the defining features
class DefiningFeatures(Timed):
    """Rule 13: one presence test per defining feature in docs/brief.md."""

    label = "render"

    def test_render_is_always_the_wire(self):
        """Defining feature 1, and the one acceptance test that decides whether A B see may
        ship at all (sketch 4.6): a bounce left in Reference mode, Blind on, Trim +6 dB nulls
        against the plugin removed."""
        hostile = {"listen": LISTEN["reference"], "blind": 1.0, "trim": 0.625,   # +6 dB
                   "mono": 1.0, "matchMode": MATCH["short"], "section": SECTION["loudest"],
                   "trimMixInstead": 1.0}
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            res = run(hostile, reference=str(Material.quiet), mix_meter="prepass",
                      non_realtime=1, out=d / "bounce.wav", seconds=3.0)
            run({"bypass": 1.0}, out=d / "removed.wav", seconds=3.0)
            bounce, _ = K.read_wav(d / "bounce.wav")
            removed, _ = K.read_wav(d / "removed.wav")
            self.assertTrue(res["product"]["render_safety"]["wire_forced"],
                            "the plugin did not notice the offline render")
            self.assertTrue((bounce == removed).all(),
                            "a bounce in Reference mode is not the mix: residual %.1f dBFS"
                            % K.null_residual(removed, bounce, align=False)["peak_dbfs"])

    def test_match_readout_says_what_it_did(self):
        """Defining feature 2: the readout states what was done, to which side, in dB -
        never a bare number."""
        res = run({"listen": LISTEN["reference"], "matchMode": MATCH["integrated"],
                   "section": SECTION["whole"], "followTransport": 0.0},
                  reference=str(Material.quiet), mix_meter="prepass", seconds=3.0)
        readout = res["product"]["match"]["readout"]
        self.assertRegex(readout, r"^(Mix|Reference) [+-]\d+\.\d dB$",
                         "the readout must name the side and carry a sign: %r" % readout)
        self.assertIn("Reference", readout)
        self.assertAlmostEqual(res["product"]["match"]["reference_gain_db"], 6.0, delta=0.15)

    def test_band_deltas_are_not_measurable_yet(self):
        """Defining feature 3 (seven band deltas) needs the shared Analysis/FFT module. The
        test exists so the gap is visible in the suite, not only in docs/status.md, and fails
        the day the module lands - which is when the real presence test gets written."""
        res = run(seconds=0.5)
        self.assertNotIn("band_deltas", res["product"],
                         "Analysis/FFT has landed: write the band-delta presence test and "
                         "remove this placeholder (docs/status.md, Waiting for Björn)")


class BriefAcceptance(Timed):
    """The brief's own acceptance tests (sketch 6), on the plugin."""

    label = "render"

    def test_live_match_lands_within_the_brief(self):
        """brief must_do: the reference matched to the mix within 0.1 LU (Integrated, Short 3 s)
        or 0.1 dB (True peak), by the plugin's own live meter (oberli::Loudness, no
        --mix-reading). Judged on the rendered audio by the kit's independent BS.1770 and 4x
        true peak, after the meter's 3 s window and the 20 ms smoother have settled: the
        reference over one full 4 s loop against the mix so far."""
        fs, seconds, settle = 48000, 12.0, 8.0
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            run({"listen": LISTEN["mix"]}, seconds=seconds, out=d / "mix.wav")
            mix, _ = K.read_wav(d / "mix.wav")
            a, b = int(settle * fs), int((settle + REF_SECONDS) * fs)
            for name, tolerance in (("integrated", 0.1), ("short", 0.1), ("truepeak", 0.1)):
                res = run({"listen": LISTEN["reference"], "matchMode": MATCH[name],
                           "section": SECTION["whole"], "followTransport": 0.0},
                          reference=str(Material.quiet), seconds=seconds, out=d / ("%s.wav" % name))
                self.assertEqual(res["applied"]["mix_reading"], "live", "the live meter did not run")
                out, _ = K.read_wav(d / ("%s.wav" % name))
                if name == "truepeak":
                    error = K.true_peak_dbtp(out[a:b], fs) - K.true_peak_dbtp(mix[:b], fs)
                elif name == "short":
                    error = K.integrated_lufs(out[a:b], fs) - K.integrated_lufs(mix[a:b], fs)
                else:
                    error = K.integrated_lufs(out[a:b], fs) - K.integrated_lufs(mix[:b], fs)
                self.assertLess(abs(error), tolerance,
                                "%s match off by %+.3f (gain %.2f dB, readout %r)"
                                % (name, error, res["product"]["match"]["reference_gain_db"],
                                   res["product"]["match"]["readout"]))

    def test_switch_does_not_overshoot(self):
        """brief quality_targets: a 1 kHz sine on both sides, 200 switches, no sample above
        peak + 0.01 dB. This is the test that chose the fade law (G1 departure 3): equal-power
        peaks +3.0086 dB here."""
        seconds = 20.0
        points = ",".join("%.4f=%d" % (0.05 + i * (seconds - 0.1) / 200.0, i % 2) for i in range(1, 201))
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "switches.wav"
            run({"matchMode": MATCH["off"], "section": SECTION["whole"], "followTransport": 0.0},
                reference=str(Material.sine), signal="sine:1000:-12", seconds=seconds,
                automate="listen:" + points, out=out)
            y, fs = K.read_wav(out)
            over = K.peak_dbfs(y) - (-12.0)
            self.assertLess(over, 0.01, "the switch overshot by %+.4f dB" % over)

    def test_loop_seam_is_not_a_step(self):
        """The section loop jumps from out back to in; the 5 ms seam fade must hide it.
        100 Hz, because `max_click_db` is absolute and a 1 kHz sine's own slope (-17.7 dB)
        would hide the seam entirely."""
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "seam.wav"
            run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                 "section": SECTION["1"], "followTransport": 0.0},
                reference=str(Material.low), signal="silence", seconds=6.0,
                marks="1,1,0.0,1.5025", out=out)
            y, fs = K.read_wav(out)
            click = K.max_click_db(y, fs)
            self.assertLess(click, -30.0, "the loop seam reads %.1f dB" % click)

    def test_at_most_two_references_are_resident(self):
        """brief quality_targets: 8 slots loaded, at most 2 decoded buffers resident."""
        res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                   "section": SECTION["whole"], "followTransport": 0.0},
                  reference=[str(Material.quiet), str(Material.other), str(Material.third)],
                  signal="silence", seconds=3.0,
                  # the slot sweeps 1 -> 2 -> 3 while the reference is playing (refSlot is a
                  # choice of eight, so the normalised step between slots is 1/7)
                  automate="refSlot:0=0.0,1=0.142857,2=0.285714")
        self.assertEqual(res["applied"]["references"], 3)
        self.assertLessEqual(res["product"]["reference"]["resident"], 2,
                             "more than two references were decoded at once")


class Transport(Timed):
    label = "render"

    def test_follow_transport_plays_and_stops_with_the_host(self):          # sketch 4.3
        playing = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                       "section": SECTION["whole"], "followTransport": 1.0},
                      reference=str(Material.quiet), signal="silence", seconds=1.0, transport=120)
        stopped = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                       "section": SECTION["whole"], "followTransport": 1.0},
                      reference=str(Material.quiet), signal="silence", seconds=1.0,
                      transport="stopped")
        self.assertTrue(playing["product"]["reference"]["playing"])
        self.assertFalse(stopped["product"]["reference"]["playing"])
        self.assertLess(stopped["levels"]["peak_dbfs"], -200.0, "the reference played while the DAW stood still")
        # free-running ignores the transport, so the reference loops with the DAW stopped
        free = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                    "section": SECTION["whole"], "followTransport": 0.0},
                   reference=str(Material.quiet), signal="silence", seconds=1.0, transport="stopped")
        self.assertTrue(free["product"]["reference"]["playing"])

    def test_transport_map_does_not_drift(self):                            # G1 departure 2
        """An impulse per second in the reference must stay on the second over 8 s."""
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "ticks.wav"
            run({"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                 "section": SECTION["whole"], "followTransport": 1.0},
                reference=str(Material.ticks), signal="silence", seconds=7.5,
                transport=120, out=out)
            y, fs = K.read_wav(out)
            hits = np.flatnonzero(np.abs(y[:, 0]) > 0.5)
            self.assertGreater(hits.size, 5, "the transport map did not play")
            drift_ms = np.max(np.abs(hits - np.round(hits / fs) * fs)) / fs * 1000.0
            self.assertLess(drift_ms, 1.0, "worst deviation %.3f ms" % drift_ms)

    def test_align_pair_shifts_the_reference(self):                         # sketch 4.3, "Align here"
        with tempfile.TemporaryDirectory() as d:
            plain, shifted = Path(d) / "plain.wav", Path(d) / "shifted.wav"
            params = {"listen": LISTEN["reference"], "matchMode": MATCH["off"],
                      "section": SECTION["whole"], "followTransport": 1.0}
            run(params, reference=str(Material.ticks), signal="silence", seconds=2.0,
                transport=120, out=plain)
            run(params, reference=str(Material.ticks), signal="silence", seconds=2.0,
                transport=120, align="1,0.0,0.5", out=shifted)
            a, fs = K.read_wav(plain)
            b, _ = K.read_wav(shifted)
            first_a = int(np.flatnonzero(np.abs(a[:, 0]) > 0.5)[0])
            first_b = int(np.flatnonzero(np.abs(b[:, 0]) > 0.5)[0])
            # The align pair says "DAW second 0 is reference second 0.5", so the reference
            # starts half a second in and its ticks arrive half a second early.
            self.assertAlmostEqual((first_a - first_b) / fs, 0.5, delta=0.002)


class Blind(Timed):
    label = "render"

    def test_blind_assignment_is_seeded(self):                              # rule 8, sketch 4.4
        a = run({"blind": 1.0}, seed=7, seconds=0.25)
        b = run({"blind": 1.0}, seed=7, seconds=0.25)
        self.assertEqual(a["product"]["listen"]["x_is_mix"], b["product"]["listen"]["x_is_mix"])
        seeds = {run({"blind": 1.0}, seed=s, seconds=0.25)["product"]["listen"]["x_is_mix"]
                 for s in (1, 2, 3, 4, 5, 6, 7, 8)}
        self.assertEqual(seeds, {True, False}, "the blind draw does not depend on the seed")

    def test_blind_hides_the_side_but_still_plays_one(self):
        res = run({"blind": 1.0, "listen": LISTEN["reference"], "matchMode": MATCH["off"],
                   "section": SECTION["whole"], "followTransport": 0.0},
                  reference=str(Material.quiet), seed=1, seconds=1.0)
        self.assertIn(res["product"]["listen"]["side"], ("mix", "reference"))

    def test_no_reference_keeps_the_switch_on_the_mix(self):                 # sketch 2.5, state
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "noref.wav"
            res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"]},
                      seconds=1.0, out=out)
            self.assertEqual(res["product"]["listen"]["side"], "mix")
            x, _ = K.read_wav(out)
            run({"bypass": 1.0}, seconds=1.0, out=Path(d) / "wire.wav")
            w, _ = K.read_wav(Path(d) / "wire.wav")
            self.assertTrue((x == w).all(), "with no reference loaded the output is not the mix")



class ReferenceLoading(Timed):
    """The plugin's own ReferenceLoader (worker decode, message-thread publish), run inline by
    the harness's --loader=plugin."""

    def test_plugin_loader_plays_what_the_harness_loader_plays(self):         # ReferenceLoader.h
        """The plugin's own loader (worker decode + message-thread publish, here run inline)
        publishes the same buffer as the harness: the two renders are bit-identical."""
        params = {"listen": LISTEN["reference"], "matchMode": MATCH["short"], "section": SECTION["loudest"],
                  "followTransport": 0.0}
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            run(params, reference=str(Material.quiet), seconds=2.0, out=d / "harness.wav")
            res = run(params, reference=str(Material.quiet), loader="plugin", seconds=2.0, out=d / "plugin.wav")
            self.assertEqual(res["applied"]["loader"], "plugin")
            self.assertTrue(res["product"]["reference"]["playing"], "the plugin's loader published nothing")
            a, _ = K.read_wav(d / "harness.wav")
            b, _ = K.read_wav(d / "plugin.wav")
            self.assertTrue((a == b).all(), "the plugin's loader built another buffer: residual %.1f dBFS"
                            % K.null_residual(a, b, align=False)["peak_dbfs"])

    def test_restored_project_plays_its_reference(self):                    # sketch 3: reopening a set
        """A saved project carries the path; reopened, the plugin decodes it again itself."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            run(state_out=d / "s.state", seconds=0.25, reference=str(Material.quiet))
            res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"], "followTransport": 0.0},
                      state_in=d / "s.state", loader="plugin", seconds=1.0)
            self.assertEqual(res["applied"]["load_errors"], None)
            self.assertTrue(res["product"]["reference"]["playing"], "the restored reference does not play")
            self.assertEqual(res["product"]["listen"]["side"], "reference")

    def test_missing_reference_says_so_and_keeps_the_mix(self):             # sketch 2.5, state
        """A project whose reference file has gone: a plain-words error for the slot, no crash,
        and the switch stays on the mix."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            gone = d / "gone.wav"
            K.write_wav(gone, np.zeros((4800, 2), dtype=np.float32), 48000)
            run(state_out=d / "s.state", seconds=0.25, reference=str(gone))
            gone.unlink()
            res = run({"listen": LISTEN["reference"], "matchMode": MATCH["off"]},
                      state_in=d / "s.state", loader="plugin", seconds=0.5)
            self.assertEqual(len(res["applied"]["load_errors"] or []), 1, res["applied"].get("load_errors"))
            self.assertFalse(res["product"]["reference"]["playing"])
            self.assertEqual(res["product"]["listen"]["side"], "mix")


class Performance(Timed):
    """rule 20. The figure that counts is `perf.py`'s on Björn's Mac at 48 k/64 on the slice
    the host runs; this is the guard that keeps the number from drifting between releases,
    and it is in `render` because 7500 blocks do not fit the fast budget (D.3.6)."""

    label = "render"

    def test_cpu_is_in_budget(self):
        res = run({"listen": LISTEN["reference"], "followTransport": 0.0,
                   "matchMode": MATCH["integrated"], "section": SECTION["whole"]},
                  signal="noise:pink:-20", seconds=10.0, profile=1, sample_rate=48000,
                  block=64, warmup=200, reference=str(Material.quiet), mix_meter="prepass")
        self.assertLess(res["profile"]["p999_pct"], 25.0)                  # rule 20: p99.9 judged
        self.assertLess(res["levels"]["dc_dbfs"], -60.0)
        print("p50 %.1f %%, p99.9 %.1f %%, max %.1f %%, over_deadline %d (reported, not judged)"
              % (res["profile"]["p50_pct"], res["profile"]["p999_pct"],
                 res["profile"]["max_pct"], res["profile"]["over_deadline"]))


class Prototype(Timed):
    """G5: the plugin against the G2 prototype, block by block, on the same material."""

    label = "render"

    def test_plugin_matches_the_prototype(self):
        fs, seconds = 48000, 4.0
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "plugin.wav"
            # a fixed mix reading on both sides, so the comparison is the chain, not the meter
            reading_lufs = -27.0
            res = run({"listen": LISTEN["reference"], "matchMode": MATCH["integrated"],
                       "section": SECTION["whole"], "followTransport": 0.0, "trim": TRIM0},
                      reference=str(Material.quiet), signal="silence", seconds=seconds,
                      mix_reading="%f" % reading_lufs, out=out)
            got, _ = K.read_wav(out)

            ref, _ = K.read_wav(Material.quiet)
            mix = np.zeros((int(seconds * fs), 2))
            analysis = P.ReferenceAnalysis(ref, fs, {P.SECTION_WHOLE: (0.0, REF_SECONDS)})
            state = P.State(listen=P.LISTEN_REFERENCE, match_mode=P.MATCH_INTEGRATED,
                            section=P.SECTION_WHOLE, follow_transport=False)
            want, info = P.render_chain(mix, fs, ref, analysis, state, block=512,
                                        mix_reading=P.MixReading(valid=True,
                                                                 integrated_lufs=reading_lufs,
                                                                 short_lufs=reading_lufs,
                                                                 true_peak_dbtp=-6.0))
            self.assertAlmostEqual(res["product"]["match"]["reference_gain_db"],
                                   info["reference_gain_db"], delta=0.01,
                                   msg="plugin and prototype disagree on the match gain")
            # The first 10 ms are excluded, and only those: the host restores Listen *after*
            # prepareToPlay, so the plugin's switch starts on the mix and fades in over 5 ms,
            # while the prototype starts settled on the side it was given. It is an
            # initialisation difference, not a difference in the chain - measured
            # 2026-11-01: residual -32.7 dBFS over the first 10 ms, -108.9 dBFS after them,
            # which is the float32 WAV floor for this level, not a DSP disagreement.
            settled = slice(int(0.010 * fs), None)
            r = K.null_residual(want[settled], got[settled], align=False)
            self.assertLess(r["peak_dbfs"], -90.0,
                            "plugin against prototype: residual %.1f dBFS" % r["peak_dbfs"])
            # and the excluded window really is a fade-in from the mix, not something else
            head = got[: int(0.005 * fs)]
            self.assertLess(K.peak_dbfs(head[: int(0.0005 * fs)]), K.peak_dbfs(head[-int(0.0005 * fs):]),
                            "the switch did not fade in at the first block")


class Goldens(Timed):
    """F.13.6: a golden per defining feature, rendered on GOLDEN_ARCH, with a sidecar."""

    label = "render"

    def test_goldens_match(self):
        sidecars = sorted(GOLDEN_DIR.glob("*.json")) if GOLDEN_DIR.exists() else []
        if not sidecars:
            self.skipTest("no goldens in %s yet" % GOLDEN_DIR)
        for sc in sidecars:
            meta = json.loads(sc.read_text())
            with self.subTest(golden=sc.stem), tempfile.TemporaryDirectory() as d:
                out = Path(d) / "now.wav"
                flags = dict(meta["flags"])
                if "reference" in flags:
                    flags["reference"] = str(getattr(Material, flags["reference"]))
                flags["out"] = out
                res = K.render(HARNESS, meta.get("params"), **flags)
                arch = res["applied"].get("build", {}).get("arch")
                kind = "same" if arch == meta.get("arch") else ("cross-feedback" if meta.get("feedback") else "cross-feedforward")
                now, fs = K.read_wav(out)
                gold, _ = K.read_wav(sc.with_suffix(".wav"))
                r = K.null_residual(gold, now, align=False)
                self.assertLess(r["peak_dbfs"], GOLDEN_PEAK_DB[kind],
                                "%s: residual against the golden (%s) - regenerate only with a "
                                "changelog line (D.3.5)" % (sc.stem, kind))


CLASSES = (Contract, RateAndBlock, Health, Properties, DefiningFeatures, BriefAcceptance,
           Transport, Blind, Performance, Prototype, Goldens)

if __name__ == "__main__":
    label = None
    if "--label" in sys.argv:
        i = sys.argv.index("--label")
        label = sys.argv[i + 1]
        del sys.argv[i:i + 2]
    if label:
        loader = unittest.TestLoader()
        suite = unittest.TestSuite()
        for cls in CLASSES:
            if cls.label == label:
                suite.addTests(loader.loadTestsFromTestCase(cls))
        t0 = time.time()
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        dt = time.time() - t0
        print("label %s: %.0f s" % (label, dt))
        sys.exit(0 if result.wasSuccessful() and (label != "fast" or dt < 3.0 * FAST_BUDGET_S) else 1)
    unittest.main(verbosity=2)
