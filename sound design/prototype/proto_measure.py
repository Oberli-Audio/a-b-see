#!/usr/bin/env python3
"""A B see - G3: measure the prototype (Tier P, track A).

One case per number in `docs/brief.md`'s quality targets and per departure recorded at G1.
Every case prints the command that produced it, the measured value, the threshold and a
verdict; `--json` writes the same for `docs/status.md`.

    python3 "sound design/prototype/proto_measure.py" --all
    python3 "sound design/prototype/proto_measure.py" --case switch-click

The cases:
  wire-null         Mix mode at Trim 0 is bit-exact (brief: thd_n = wire)
  render-null       an offline render is the mix, whatever the state was (defining feature 1)
  match-integrated  the mix as its own reference at -6 dB reads Reference +6.0 dB (+/-0.1 LU)
  match-short       the same in Short 3 s mode
  match-truepeak    the same in True peak mode (+/-0.1 dB)
  match-target      "Trim mix instead": both sides reach matchTarget
  switch-click      200 switches on a 1 kHz sine, no sample above peak + 0.01 dB; 5 vs 10 ms
  loop-seam         the section loop's seam is not a step
  transport-drift   following the transport for 60 s does not slide the reference
  kweighting-rates  the same signal reads the same loudness at all six supported rates
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, os.environ.get("DSPKIT", str(HERE.parents[1] / "scripts" / "dsp-kit")))
import dspkit as K  # noqa: E402
import absee_proto as P  # noqa: E402

FS = 48000
RATES = (44100, 48000, 88200, 96000, 176400, 192000)


class Result:
    def __init__(self, case, value, unit, threshold, ok, command, note=""):
        self.case, self.value, self.unit = case, value, unit
        self.threshold, self.ok, self.command, self.note = threshold, ok, command, note

    def row(self):
        return "%-17s %10s %-6s  %-26s %-4s %s" % (
            self.case,
            ("%.3f" % self.value) if isinstance(self.value, float) else str(self.value),
            self.unit, self.threshold, "pass" if self.ok else "FAIL", self.note)

    def as_dict(self):
        return dict(case=self.case, value=self.value, unit=self.unit,
                    threshold=self.threshold, ok=bool(self.ok),
                    command=self.command, note=self.note)


def cmd(case):
    return 'python3 "sound design/prototype/proto_measure.py" --case %s' % case


# ---------------------------------------------------------------------------- cases
def case_wire_null():
    """Mix mode, Match Integrated, Trim 0: the output must be the input, bit for bit."""
    mix = P.pink(int(4.0 * FS), FS, -20.0, seed=1)
    ref = mix * P.db_to_gain(-6.0)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, 4.0)})
    y, _ = P.render_chain(mix, FS, ref, an, P.State(section=P.SECTION_WHOLE, follow_transport=False))
    exact = bool(np.array_equal(y, mix))
    r = K.null_residual(mix, y, align=False)
    return Result("wire-null", r["peak_dbfs"], "dBFS", "bit-exact (-400)", exact, cmd("wire-null"),
                  "bit-exact" if exact else "NOT bit-exact")


def case_render_null():
    """Defining feature 1: a bounce left in Reference mode, Blind on, Trim +6 dB nulls
    against the plugin removed."""
    mix = P.pink(int(4.0 * FS), FS, -20.0, seed=1)
    ref = P.pink(int(4.0 * FS), FS, -14.0, seed=7)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, 4.0)})
    st = P.State(listen=P.LISTEN_REFERENCE, blind=True, trim_db=6.0, mono=True,
                 match_mode=P.MATCH_SHORT, section=P.SECTION_WHOLE, follow_transport=False,
                 non_realtime=True)
    y, info = P.render_chain(mix, FS, ref, an, st)
    exact = bool(np.array_equal(y, mix))
    r = K.null_residual(mix, y, align=False)
    return Result("render-null", r["peak_dbfs"], "dBFS", "bit-exact (-400)",
                  exact and info["wire_forced"], cmd("render-null"),
                  "wire forced" if info["wire_forced"] else "WIRE NOT FORCED")


def _match_case(name, mode, tol, unit):
    """The brief's own case: the reference is the mix itself at -6 dB, so a correct match
    renders the reference back at the mix's level and the output nulls against the mix."""
    seconds = 20.0
    mix = P.pink(int(seconds * FS), FS, -20.0, seed=1)
    ref = mix * P.db_to_gain(-6.0)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, seconds)})
    st = P.State(listen=P.LISTEN_REFERENCE, match_mode=mode, section=P.SECTION_WHOLE,
                 follow_transport=False)
    y, info = P.render_chain(mix, FS, ref, an, st)
    err = abs(info["reference_gain_db"] - 6.0)
    # The rendered proof: the reference *is* the mix at -6 dB and starts at the same sample,
    # so once the smoother has settled the output must be the mix again. A pure gain error of
    # 0.1 dB leaves a residual at -38.7 dBFS, so the residual is the gain error, measured on
    # the audio. The window stops before the section's seam fade (the loop joins the file's
    # end to its start, which is not the mix).
    tail = slice(int(10.0 * FS), int((seconds - 0.05) * FS))
    resid = K.null_residual(mix[tail], y[tail], align=False)["peak_dbfs"]
    return Result(name, err, unit, "<= %.2f %s" % (tol, unit), err <= tol, cmd(name),
                  "readout %r, residual of the rendered output against the mix %.1f dBFS "
                  "(0.1 dB of gain error = -38.7 dBFS)" % (info["readout"], resid))


def case_match_integrated():
    return _match_case("match-integrated", P.MATCH_INTEGRATED, 0.1, "LU")


def case_match_short():
    return _match_case("match-short", P.MATCH_SHORT, 0.1, "LU")


def case_match_truepeak():
    return _match_case("match-truepeak", P.MATCH_TRUE_PEAK, 0.1, "dB")


def case_match_target():
    """Trim mix instead: both sides are brought to matchTarget, so a -8 LUFS master and a
    -20 LUFS reference meet at -14."""
    seconds = 20.0
    mix = P.pink(int(seconds * FS), FS, -8.0, seed=1)
    ref = P.pink(int(seconds * FS), FS, -20.0, seed=2)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, seconds)})
    target = -14.0
    out = {}
    for side, listen in (("mix", P.LISTEN_MIX), ("reference", P.LISTEN_REFERENCE)):
        st = P.State(listen=listen, match_mode=P.MATCH_INTEGRATED, section=P.SECTION_WHOLE,
                     follow_transport=False, trim_mix_instead=True, match_target_lufs=target)
        y, _ = P.render_chain(mix, FS, ref, an, st)
        out[side] = K.integrated_lufs(y[int(10.0 * FS):], FS)
    err = max(abs(out["mix"] - target), abs(out["reference"] - target))
    return Result("match-target", err, "LU", "<= 0.30 LU", err <= 0.30, cmd("match-target"),
                  "mix %.2f LUFS, reference %.2f LUFS, target %.1f" % (out["mix"], out["reference"], target))


def _switch_click(fade_s, law, correlated=True, seed=11):
    """The brief's test: a 1 kHz sine on both sides, 200 switches. The sine is the hard case
    for a fade law, because both sides are perfectly correlated."""
    seconds = 20.0
    n = int(seconds * FS)
    mix = P.sine(n, FS, 1000.0, -12.0)
    ref = mix.copy() if correlated else P.pink(n, FS, -12.0 - 3.0, seed=seed)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, seconds)})
    switches = [(0.05 + i * (seconds - 0.1) / 200.0, i % 2) for i in range(1, 201)]
    saved_s, saved_law = P.XFADE_S, P.XFADE_LAW
    P.XFADE_S, P.XFADE_LAW = fade_s, law
    try:
        st = P.State(match_mode=P.MATCH_OFF, section=P.SECTION_WHOLE, follow_transport=False)
        y, _ = P.render_chain(mix, FS, ref, an, st, switch_at=switches)
    finally:
        P.XFADE_S, P.XFADE_LAW = saved_s, saved_law
    over_db = K.peak_dbfs(y) - max(K.peak_dbfs(mix), K.peak_dbfs(ref))
    return over_db, K.max_click_db(y, FS), y


def case_switch_click():
    over5, click5, _ = _switch_click(0.005, P.XFADE_LAW)
    over10, click10, _ = _switch_click(0.010, P.XFADE_LAW)
    power5, _, _ = _switch_click(0.005, "equal-power")
    # what equal-gain costs on the normal case: two uncorrelated, level-matched sides
    _, _, y_unc = _switch_click(0.005, P.XFADE_LAW, correlated=False)
    ok = over5 <= 0.01 and (click5 - click10) <= 0.5
    return Result("switch-click", over5, "dB", "<= 0.01 dB over peak", ok, cmd("switch-click"),
                  "%s 5 ms: %+.4f dB over peak, max_click %.1f dB | same law 10 ms: %+.4f dB, "
                  "%.1f dB (departure 1 holds while 5 ms is within 0.5 dB of 10 ms) | "
                  "equal-power 5 ms would be %+.4f dB (departure 3) | uncorrelated sides peak "
                  "%+.2f dBFS"
                  % (P.XFADE_LAW, over5, click5, over10, click10, power5, K.peak_dbfs(y_unc)))


def case_loop_seam():
    """A section of a 100 Hz sine looped: the seam must not be a step.

    100 Hz, not 1 kHz: `max_click_db` is absolute, and a sine's own sample-to-sample slope is
    its floor - at 1 kHz that floor is -17.7 dB, which hides the seam entirely, at 100 Hz it
    is -37.7 dB. The section is 150.25 periods long, so a hard loop would jump a quarter
    period: the worst case, not a lucky zero crossing.
    """
    seconds, section = 6.0, 1.5025
    mix = P.pink(int(seconds * FS), FS, -60.0, seed=3)        # near-silent: the seam is what we hear
    ref = P.sine(int(math.ceil(section * FS)), FS, 100.0, -12.0)
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, section)})
    st = P.State(listen=P.LISTEN_REFERENCE, match_mode=P.MATCH_OFF, section=P.SECTION_WHOLE,
                 follow_transport=False)
    y, _ = P.render_chain(mix, FS, ref, an, st)
    click = K.max_click_db(y, FS)
    hard = np.vstack([ref[:int(section * FS)]] * 5)[:len(y)]   # the same loop without the fade
    return Result("loop-seam", click, "dB", "<= -30 dB max_click", click <= -30.0, cmd("loop-seam"),
                  "the same loop without the seam fade reads %.1f dB; the sine's own slope "
                  "floor is %.1f dB" % (K.max_click_db(hard, FS), 20 * math.log10(2 * math.sin(math.pi * 100.0 / FS))))


def case_transport_drift():
    """Follow transport for 60 s at 120 bpm: an impulse train in the reference must stay on
    the second, so the map does not slide (departure 2's rejecting measurement)."""
    seconds, section = 60.0, 10.0
    mix = np.zeros((int(seconds * FS), 2))
    ref = np.zeros((int(section * FS), 2))
    for s in range(int(section)):
        ref[s * FS] = 1.0                                       # one impulse per second
    an = P.ReferenceAnalysis(ref, FS, {P.SECTION_WHOLE: (0.0, section)})
    st = P.State(listen=P.LISTEN_REFERENCE, match_mode=P.MATCH_OFF, section=P.SECTION_WHOLE,
                 follow_transport=True)
    y, _ = P.render_chain(mix, FS, ref, an, st, block=512, transport_bpm=120.0)
    hits = np.flatnonzero(np.abs(y[:, 0]) > 0.5)
    if hits.size == 0:
        return Result("transport-drift", float("inf"), "ms", "<= 1 ms over 60 s", False,
                      cmd("transport-drift"), "no impulse found: the transport map did not play")
    expected = np.round(hits / FS) * FS
    drift_ms = float(np.max(np.abs(hits - expected)) / FS * 1000.0)
    return Result("transport-drift", drift_ms, "ms", "<= 1 ms over 60 s", drift_ms <= 1.0,
                  cmd("transport-drift"), "%d impulses over %.0f s, worst deviation %.3f ms"
                  % (hits.size, seconds, drift_ms))


def case_kweighting_rates():
    """Rule 17 / A.9: the meter the match depends on must read the same at every supported
    rate. Measured on `dspkit` (the reference implementation the shared module must match)."""
    readings = {}
    for fs in RATES:
        n = int(10.0 * fs)
        t = np.arange(n) / fs
        # EBU Tech 3341 case 1: a 1 kHz sine at -23 dBFS in both channels reads -23.0 LUFS
        # (BS.1770 sums the two channel powers at weight 1 and the K-weighting's +0.691 dB at
        # 1 kHz cancels the -0.691 offset); no stereo correction belongs here.
        s = np.sin(2.0 * math.pi * 1000.0 * t) * P.db_to_gain(-23.0)
        x = np.stack([s, s], axis=1)
        readings[fs] = K.integrated_lufs(x, fs)
    spread = max(readings.values()) - min(readings.values())
    worst = max(abs(v + 23.0) for v in readings.values())
    return Result("kweighting-rates", worst, "LU", "<= 0.1 LU vs -23 LUFS", worst <= 0.1,
                  cmd("kweighting-rates"),
                  " ".join("%d:%.2f" % (fs, v) for fs, v in readings.items()))


CASES = {
    "wire-null": case_wire_null,
    "render-null": case_render_null,
    "match-integrated": case_match_integrated,
    "match-short": case_match_short,
    "match-truepeak": case_match_truepeak,
    "match-target": case_match_target,
    "switch-click": case_switch_click,
    "loop-seam": case_loop_seam,
    "transport-drift": case_transport_drift,
    "kweighting-rates": case_kweighting_rates,
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", action="append", choices=sorted(CASES), help="repeatable")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--json", help="write the results as JSON")
    a = ap.parse_args(argv)
    names = sorted(CASES) if (a.all or not a.case) else a.case
    print("%-17s %10s %-6s  %-26s %-4s %s" % ("case", "value", "unit", "threshold", "", "note"))
    results = []
    for name in names:
        r = CASES[name]()
        results.append(r)
        print(r.row())
    if a.json:
        Path(a.json).write_text(json.dumps([r.as_dict() for r in results], indent=2) + "\n")
    bad = [r.case for r in results if not r.ok]
    print("\n%d/%d pass" % (len(results) - len(bad), len(results)) + (("; failed: " + ", ".join(bad)) if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
