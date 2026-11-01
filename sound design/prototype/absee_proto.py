#!/usr/bin/env python3
"""A B see - G2 prototype of the matching chain (Tier P, track A).

Track A prototypes are numpy, not Faust (F.15.1): everything that decides how this plugin
sounds is a *measurement* plus a gain, and the measurement is BS.1770, which `dspkit` already
implements and `test_dspkit.py` already proves against the EBU Tech 3341/3342 cases. The
prototype is written **in the structure of the port** (F.8), one class per C++ module:

    MatchEngine      -> Source/MatchEngine.h      the match policy on top of the meter
    Crossfade        -> Source/Crossfade.h        5 ms equal-power switch
    ReferencePlayer  -> Source/ReferencePlayer.h  section loop, seam fade, transport map
    OutputStage      -> Source/PluginProcessor    trim, mono
    render_chain     -> ABSeeAudioProcessor::processBlock

so G4 is a hand port of structure, not of generated code, and G5 can null the two against
each other block by block.

What the prototype does NOT do, by design: it does not implement a loudness meter (that is
the shared `oberli::Loudness`; here the readings come from `dspkit`), it does not decode,
resample or cache reference files (that is the shared `oberli::References`), and it draws
nothing.

    python3 "sound design/prototype/absee_proto.py" --demo        # the reference renders
    python3 "sound design/prototype/absee_proto.py" --help
"""
import argparse
import math
import os
import sys
from pathlib import Path

import numpy as np
from scipy import signal as sp_signal

sys.path.insert(0, os.environ.get("DSPKIT", str(Path(__file__).resolve().parents[2] / "scripts" / "dsp-kit")))
import dspkit as K  # noqa: E402


def one_pole(x, coeff, state):
    """y[n] = (1-a) x[n] + a y[n-1], vectorised. Returns (y, new_state).

    The C++ port runs this per sample in `processBlock`; here it is one `lfilter` call so a
    measurement sweep is seconds, not minutes. Same difference equation, so the two null.
    """
    y, zf = sp_signal.lfilter([1.0 - coeff], [1.0, -coeff], x, zi=[coeff * state])
    return y, float(y[-1]) if len(y) else state

# ---------------------------------------------------------------------------- constants
# Every time is in seconds and converted against the rate in prepare() (rule 12). These are
# the numbers the C++ port repeats beside its code; the reasons are in docs/status.md.
XFADE_S = 0.005          # brief: 5 ms switch (departure 1 from rule 2)
# The fade law. The brief says "equal-power", but its own acceptance test (1 kHz sine on both
# sides, 200 switches, no sample above peak + 0.01 dB) cannot be met that way: with the same
# signal on both sides sin+cos peaks at +3.01 dB, and two masters at -1 dBTP would clip while
# monitoring. Equal-gain costs a 3.01 dB dip at the midpoint of the fade when the two sides
# are uncorrelated, which lasts well under a millisecond. Measured both ways:
# `proto_measure.py --case switch-click`. Departure 3, raised with the owner.
XFADE_LAW = "equal-gain"  # "equal-gain" | "equal-power"
SEAM_S = 0.005           # the section loop's seam fade, same constant as the switch
SEAM_LAW = "equal-power"  # the seam joins two *uncorrelated* points of the same file
MATCH_TAU_S = 0.020      # brief 4.1: one-pole smoother on the match gain, 20 ms
MATCH_HYSTERESIS_LU = 0.1  # Integrated mode only changes when the reading moves by this much
SHORT_WINDOW_S = 3.0     # LUFS-S window (BS.1770)
MONO_SUM_DB = -3.0       # sketch 4.1: mono sums at -3 dB
LOUDEST_WINDOW_S = 10.0  # "Loudest 10 s" section

LISTEN_MIX, LISTEN_REFERENCE = 0, 1
MATCH_INTEGRATED, MATCH_SHORT, MATCH_TRUE_PEAK, MATCH_OFF = 0, 1, 2, 3
SECTION_WHOLE, SECTION_LOUDEST = 0, 5          # 1..4 are the user markers
MATCH_MODE_NAMES = ("Integrated", "Short 3 s", "True peak", "Off")


def db_to_gain(d):
    return 10.0 ** (d / 20.0)


# ---------------------------------------------------------------------------- analysis seam
class ReferenceAnalysis:
    """What `oberli::References` computes **once on load**, off the audio thread, and caches
    in the shared References folder. The prototype computes it with `dspkit`; the plugin
    never computes it in `processBlock`."""

    def __init__(self, audio, fs, sections=None):
        self.fs = fs
        self.integrated_lufs = K.integrated_lufs(audio, fs)
        self.true_peak_dbtp = K.true_peak_dbtp(audio, fs)
        self.sections = {}
        marks = dict(sections or {})
        marks.setdefault(SECTION_WHOLE, (0.0, len(audio) / fs))
        marks.setdefault(SECTION_LOUDEST, loudest_window(audio, fs))
        for idx, (a, b) in marks.items():
            seg = audio[int(round(a * fs)): int(round(b * fs))]
            self.sections[idx] = dict(
                in_s=float(a), out_s=float(b),
                integrated_lufs=K.integrated_lufs(seg, fs) if len(seg) else float("-inf"),
                true_peak_dbtp=K.true_peak_dbtp(seg, fs) if len(seg) else -400.0,
            )

    def section(self, idx):
        return self.sections.get(idx, self.sections[SECTION_WHOLE])


def loudest_window(audio, fs, window_s=LOUDEST_WINDOW_S):
    """The window with the highest short-term loudness, found on load (sketch 4.3)."""
    total_s = len(audio) / fs
    if total_s <= window_s:
        return (0.0, total_s)
    st = K.loudness_blocks(audio, fs, min(window_s, SHORT_WINDOW_S), 0.1)
    if st.size == 0:
        return (0.0, min(window_s, total_s))
    start = float(np.argmax(st)) * 0.1
    start = min(start, total_s - window_s)
    return (max(0.0, start), max(0.0, start) + window_s)


class MixReading:
    """What the shared `oberli::Loudness` publishes for the mix side: the live readings on
    the same meter the readout shows. `valid` is false until the meter has a gate block, and
    the match gain then stays at 0 dB (A.0: display and measurement are separate paths; this
    is the measurement path)."""

    def __init__(self, valid=False, integrated_lufs=float("-inf"),
                 short_lufs=float("-inf"), true_peak_dbtp=-400.0):
        self.valid = valid
        self.integrated_lufs = integrated_lufs
        self.short_lufs = short_lufs
        self.true_peak_dbtp = true_peak_dbtp


class OfflineMixMeter:
    """Stand-in for the streaming meter `oberli::Loudness`: it answers only from the material
    pushed so far, which is what a streaming meter has seen, but it precomputes the
    K-weighted gating blocks of the whole mix once so a measurement sweep is cheap.

    Integrated = gated, since the section started (brief 4.2: reset on transport stop or
    section change); Short = the 3 s window ending now; true peak = the running maximum.
    The block grid is BS.1770's: 400 ms blocks at a 100 ms hop.
    """

    HOP_S = 0.1
    BLOCK_S = 0.4

    def __init__(self, fs, full_mix):
        self.fs = fs
        self.count = 0
        x = K.as_2d(np.asarray(full_mix, dtype=np.float64))
        self.hop = int(round(self.HOP_S * fs))
        self.gate = int(round(self.BLOCK_S * fs))
        self.lk = K.loudness_blocks(x, fs, self.BLOCK_S, self.HOP_S)      # one Lk per block
        self.block_end = np.arange(len(self.lk)) * self.hop + self.gate   # samples consumed
        # true peak per hop chunk, so the running maximum is a prefix maximum
        chunks = max(1, int(math.ceil(len(x) / self.hop)))
        tp = [K.true_peak_dbtp(x[i * self.hop:(i + 1) * self.hop], fs) for i in range(chunks)]
        self.tp_running = np.maximum.accumulate(np.array(tp)) if chunks else np.array([-400.0])

    def reset(self):
        self.count = 0

    def push(self, block):
        self.count += len(block)

    @staticmethod
    def _gated(lk):
        lk = lk[lk > -70.0]
        if lk.size == 0:
            return float("-inf")
        rel = 10.0 * np.log10(np.mean(10.0 ** (lk / 10.0))) - 10.0
        g = lk[lk > rel]
        return float(10.0 * np.log10(np.mean(10.0 ** (g / 10.0)))) if g.size else float("-inf")

    def read(self):
        done = self.block_end <= self.count
        if not done.any():
            return MixReading(valid=False)
        lk = self.lk[done]
        n_short = max(1, int(round(SHORT_WINDOW_S / self.HOP_S)) - 3)     # 3 s of 400 ms blocks
        chunk = min(len(self.tp_running) - 1, max(0, self.count // self.hop - 1))
        return MixReading(
            valid=True,
            integrated_lufs=self._gated(lk),
            short_lufs=self._gated(lk[-n_short:]),
            true_peak_dbtp=float(self.tp_running[chunk]),
        )


# ---------------------------------------------------------------------------- the modules
class MatchEngine:
    """The match policy. Takes the two readings, returns the gain for each side in dB.

    Integrated: the difference, changed only when the reading has moved by more than
    MATCH_HYSTERESIS_LU (brief 4.2: "it only changes when the measurement changes by more
    than 0.1 LU"), so a converging integrated reading does not creep.
    Short 3 s: the difference, every block, smoothed - the user watches it move.
    True peak: the difference of the two true peaks in dB.
    Off: raw levels, 0 dB on both sides.

    With `trim_mix_instead` both sides are brought to `match_target` instead of the
    reference being brought to the mix.
    """

    def __init__(self, fs):
        self.fs = fs
        self.coeff = math.exp(-1.0 / (MATCH_TAU_S * fs))     # one-pole, tau in seconds
        self.ref_db = 0.0
        self.mix_db = 0.0
        self.ref_target_db = 0.0
        self.mix_target_db = 0.0
        self.held_mix_lufs = None

    def reset(self):
        self.ref_db = self.mix_db = 0.0
        self.ref_target_db = self.mix_target_db = 0.0
        self.held_mix_lufs = None

    def targets(self, mode, mix, ref_section, trim_mix_instead, match_target_lufs):
        """The unsmoothed targets (A.0: the exported numbers come from the unsmoothed path)."""
        if mode == MATCH_OFF:
            return 0.0, 0.0
        if trim_mix_instead:
            if mode == MATCH_TRUE_PEAK:
                if not mix.valid:
                    return 0.0, 0.0
                # true peak has no "target" in LUFS: both sides go to the target's headroom
                return match_target_lufs - ref_section["true_peak_dbtp"], match_target_lufs - mix.true_peak_dbtp
            ref_lufs = ref_section["integrated_lufs"]
            mix_lufs = mix.integrated_lufs if mode == MATCH_INTEGRATED else mix.short_lufs
            if not mix.valid or not np.isfinite(mix_lufs) or not np.isfinite(ref_lufs):
                return 0.0, 0.0
            return match_target_lufs - ref_lufs, match_target_lufs - mix_lufs
        if not mix.valid:
            return 0.0, 0.0
        if mode == MATCH_TRUE_PEAK:
            if not np.isfinite(mix.true_peak_dbtp) or not np.isfinite(ref_section["true_peak_dbtp"]):
                return 0.0, 0.0
            return mix.true_peak_dbtp - ref_section["true_peak_dbtp"], 0.0
        mix_lufs = mix.integrated_lufs if mode == MATCH_INTEGRATED else mix.short_lufs
        ref_lufs = ref_section["integrated_lufs"]
        if not np.isfinite(mix_lufs) or not np.isfinite(ref_lufs):
            return 0.0, 0.0
        if mode == MATCH_INTEGRATED:
            if self.held_mix_lufs is None or abs(mix_lufs - self.held_mix_lufs) > MATCH_HYSTERESIS_LU:
                self.held_mix_lufs = mix_lufs
            mix_lufs = self.held_mix_lufs
        return mix_lufs - ref_lufs, 0.0

    def process(self, n, mode, mix, ref_section, trim_mix_instead, match_target_lufs):
        """n samples of smoothed gain for the reference and the mix side (linear)."""
        self.ref_target_db, self.mix_target_db = self.targets(
            mode, mix, ref_section, trim_mix_instead, match_target_lufs)
        ref, self.ref_db = one_pole(np.full(n, self.ref_target_db), self.coeff, self.ref_db)
        mix_g, self.mix_db = one_pole(np.full(n, self.mix_target_db), self.coeff, self.mix_db)
        return db_to_gain(ref), db_to_gain(mix_g)

    def readout(self):
        """Defining feature 2: what was done, to which side, in dB - never a bare number."""
        if abs(self.ref_target_db) >= abs(self.mix_target_db):
            side, value = "Reference", self.ref_target_db
        else:
            side, value = "Mix", self.mix_target_db
        return "%s %+.1f dB" % (side, value)


def fade_gains(pos, law):
    """The two gains of a crossfade at position `pos` (0 = side A, 1 = side B).

    equal-gain: a + b == 1, so identical material keeps its level and nothing overshoots.
    equal-power: a^2 + b^2 == 1, so uncorrelated material keeps its power.
    Both give exactly (1, 0) at 0 and (0, 1) at 1, which is what makes Mix mode a wire.
    """
    if law == "equal-power":
        return np.cos(pos * math.pi / 2.0), np.sin(pos * math.pi / 2.0)
    return 1.0 - pos, pos


class Crossfade:
    """Crossfade between side A (mix) and side B (reference), XFADE_LAW over XFADE_S.

    The switch is started at a block boundary (brief 4.1). At rest the gains are exactly
    1.0 and 0.0, so Mix mode is a wire.
    """

    def __init__(self, fs, fade_s=None, law=None):
        fade_s = XFADE_S if fade_s is None else fade_s
        self.law = XFADE_LAW if law is None else law
        self.step = 1.0 / max(1.0, fade_s * fs)
        self.pos = 0.0        # 0 = mix, 1 = reference
        self.target = 0.0

    def reset(self, side=0.0):
        self.pos = self.target = float(side)

    def set_target(self, side):
        self.target = float(side)

    @property
    def settled(self):
        return self.pos == self.target

    def process(self, n):
        p, t, s = self.pos, self.target, self.step
        if p == t:
            pos = np.full(n, p)
        else:
            direction = 1.0 if t > p else -1.0
            pos = p + direction * s * np.arange(1, n + 1)
            pos = np.clip(pos, min(p, t), max(p, t))
        self.pos = float(pos[-1]) if n else p
        return fade_gains(pos, self.law)


class ReferencePlayer:
    """Reads the RAM buffer of the active slot with a section loop.

    The buffer is at the session rate (resampled on load by `oberli::References`, never per
    block); a buffer at another rate is treated as not ready and the reference is silent.
    The loop seam is faded over SEAM_S by reading the buffer twice, so the jump from `out`
    back to `in` is not a step.

    Free-running: the playhead advances with the render and wraps inside the section.
    Follow transport: the position comes from the DAW clock through the align pair
    (DAW seconds, reference seconds), so one DAW second is one reference second
    (departure 2); with the DAW stopped the reference stops.
    """

    def __init__(self, fs):
        self.fs = fs
        self.audio = None
        self.buffer_fs = None
        self.pos_s = 0.0
        self.align_daw_s = 0.0
        self.align_ref_s = 0.0

    @property
    def ready(self):
        return self.audio is not None and self.buffer_fs == self.fs and len(self.audio) > 0

    def load(self, audio, buffer_fs):
        self.audio = K.as_2d(audio) if audio is not None else None
        self.buffer_fs = buffer_fs

    def reset(self):
        self.pos_s = 0.0

    def _read(self, start_sample, n, section_in, section_len):
        """n samples from the section, wrapping at the seam with a SEAM_S equal-power fade."""
        out = np.zeros((n, 2))
        if not self.ready or section_len <= 0:
            return out
        src = self.audio
        in_i = int(round(section_in * self.fs))
        len_i = max(1, int(round(section_len * self.fs)))
        seam_i = min(int(round(SEAM_S * self.fs)), max(1, len_i // 2))
        idx = (start_sample + np.arange(n)) % len_i
        take = np.clip(in_i + idx, 0, len(src) - 1)
        out[:, :src.shape[1]] = src[take, :2] if src.shape[1] >= 2 else src[take, :1]
        if src.shape[1] == 1:
            out[:, 1] = out[:, 0]
        # the seam: the last seam_i samples of the section fade into the section's start
        tail = idx >= (len_i - seam_i)
        if tail.any():
            f = (idx[tail] - (len_i - seam_i)) / float(seam_i)          # 0 -> 1
            head = np.clip(in_i + (idx[tail] - (len_i - seam_i)), 0, len(src) - 1)
            nxt = np.zeros((int(tail.sum()), 2))
            nxt[:, :min(2, src.shape[1])] = src[head, :2] if src.shape[1] >= 2 else src[head, :1]
            if src.shape[1] == 1:
                nxt[:, 1] = nxt[:, 0]
            gm, gn = fade_gains(f, SEAM_LAW)
            out[tail] = out[tail] * gm[:, None] + nxt * gn[:, None]
        return out

    def render(self, n, section, follow_transport, transport):
        """`section` is (in_s, out_s); `transport` is None (no host) or
        (playing, time_s)."""
        in_s, out_s = section
        length = max(0.0, out_s - in_s)
        if not self.ready or length <= 0.0:
            return np.zeros((n, 2))
        if follow_transport:
            if transport is None or not transport[0]:
                return np.zeros((n, 2))                      # the DAW stands still: so do we
            daw = transport[1] - self.align_daw_s
            offset = (self.align_ref_s - in_s) + daw
            start = int(round((offset % length) * self.fs))
        else:
            start = int(round(self.pos_s * self.fs))
            self.pos_s = ((self.pos_s * self.fs + n) % max(1, round(length * self.fs))) / self.fs
        return self._read(start, n, in_s, length)


class OutputStage:
    """Trim (shared by both sides, after the crossfade) and the mono sum. Trim is smoothed in
    the dB domain with the table's tau (parameters.csv: trim, 20 ms, `db`)."""

    def __init__(self, fs, tau_s=0.020):
        self.coeff = math.exp(-1.0 / (tau_s * fs))
        self.db = 0.0

    def reset(self, db=0.0):
        self.db = float(db)

    def process(self, x, trim_db, mono):
        n = len(x)
        g, self.db = one_pole(np.full(n, float(trim_db)), self.coeff, self.db)
        y = x * db_to_gain(g)[:, None]
        if mono:
            m = (y[:, 0] + y[:, 1]) * db_to_gain(MONO_SUM_DB)
            y = np.stack([m, m], axis=1)
        return y


# ---------------------------------------------------------------------------- the chain
class State:
    """The host parameters (parameters.csv) plus the plugin state the harness sets."""

    def __init__(self, **kw):
        self.listen = LISTEN_MIX
        self.match_mode = MATCH_INTEGRATED
        self.blind = False
        self.trim_db = 0.0
        self.mono = False
        self.follow_transport = True
        self.trim_mix_instead = False
        self.section = SECTION_LOUDEST
        self.match_target_lufs = -14.0
        self.bypass = False
        # not parameters
        self.non_realtime = False
        self.safe_on_render = True
        self.align_daw_s = 0.0
        self.align_ref_s = 0.0
        for k, v in kw.items():
            if not hasattr(self, k):
                raise KeyError("unknown state field %r" % k)
            setattr(self, k, v)


def render_chain(mix, fs, reference, analysis, state, block=512, switch_at=None, transport_bpm=None,
                 mix_reading=None):
    """The prototype of `processBlock`, block by block.

    switch_at: list of (seconds, listen) switches applied at the next block boundary.
    transport_bpm: a playing host clock; None means no playhead at all.
    mix_reading: a fixed `MixReading` instead of the offline meter. The plugin's harness
        injects a constant reading the same way (`--mix-reading`) while the shared meter is
        missing, so with the same constant on both sides the G5 null test compares the match
        *policy* and nothing else.
    Returns (output, info) with info carrying the readout and the gains, the way the
    harness's `product` object will.
    """
    mix = K.as_2d(mix).astype(np.float64)
    if mix.shape[1] == 1:
        mix = np.hstack([mix, mix])
    n_total = len(mix)
    out = np.zeros((n_total, 2))

    # render safety (sketch 4.6, structural decision 3): offline render is the wire, always
    if state.non_realtime and state.safe_on_render:
        return mix.copy(), dict(wire_forced=True, readout="Reference +0.0 dB",
                                reference_gain_db=0.0, mix_gain_db=0.0, listen="Mix")
    if state.bypass:
        return mix.copy(), dict(wire_forced=False, bypassed=True, readout="Reference +0.0 dB",
                                reference_gain_db=0.0, mix_gain_db=0.0, listen="Mix")

    player = ReferencePlayer(fs)
    player.load(reference, fs if reference is not None else None)
    player.align_daw_s, player.align_ref_s = state.align_daw_s, state.align_ref_s
    match = MatchEngine(fs)
    xfade = Crossfade(fs)
    xfade.reset(state.listen)
    output = OutputStage(fs)
    output.reset(state.trim_db)
    meter = OfflineMixMeter(fs, mix)
    section = analysis.section(state.section) if analysis is not None else dict(
        in_s=0.0, out_s=0.0, integrated_lufs=float("-inf"), true_peak_dbtp=-400.0)
    switches = sorted(switch_at or [])
    next_switch = 0

    pos = 0
    while pos < n_total:
        n = min(block, n_total - pos)
        now = pos / fs
        while next_switch < len(switches) and switches[next_switch][0] <= now:
            xfade.set_target(switches[next_switch][1])
            next_switch += 1
        blk = mix[pos:pos + n]
        meter.push(blk)
        reading = mix_reading if mix_reading is not None else meter.read()
        transport = (True, now) if transport_bpm else None
        ref_blk = player.render(n, (section["in_s"], section["out_s"]),
                                state.follow_transport, transport)
        ref_g, mix_g = match.process(n, state.match_mode, reading, section,
                                     state.trim_mix_instead, state.match_target_lufs)
        gm, gr = xfade.process(n)
        mixed = blk * (gm * mix_g)[:, None] + ref_blk * (gr * ref_g)[:, None]
        out[pos:pos + n] = output.process(mixed, state.trim_db, state.mono)
        pos += n

    listen_name = "Reference" if xfade.pos > 0.5 else "Mix"
    if state.blind:
        listen_name = "Y" if xfade.pos > 0.5 else "X"
    return out, dict(wire_forced=False, readout=match.readout(),
                     reference_gain_db=match.ref_target_db, mix_gain_db=match.mix_target_db,
                     listen=listen_name, section_in_s=section["in_s"], section_out_s=section["out_s"])


# ---------------------------------------------------------------------------- test material
def pink(n, fs, dbfs=-20.0, seed=1):
    """The harness's pink noise (Paul Kellet), seeded (rule 8)."""
    rng = np.random.default_rng(seed)
    w = rng.uniform(-1.0, 1.0, (n, 2))
    b0 = b1 = b2 = np.zeros(2)
    out = np.empty((n, 2))
    for i in range(n):
        b0 = 0.99765 * b0 + w[i] * 0.0990460
        b1 = 0.96300 * b1 + w[i] * 0.2965164
        b2 = 0.57000 * b2 + w[i] * 1.0526913
        out[i] = 0.18 * (b0 + b1 + b2 + w[i] * 0.1848)
    return out * db_to_gain(dbfs)


def sine(n, fs, hz=1000.0, dbfs=-12.0):
    t = np.arange(n) / fs
    s = np.sin(2.0 * math.pi * hz * t) * db_to_gain(dbfs)
    return np.stack([s, s], axis=1)


def demo(out_dir, fs=48000):
    """The reference renders G2 commits: the switch, the match and the loop seam, on
    material the harness can make too (pink noise and a 1 kHz sine)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    n = int(8.0 * fs)
    mix = pink(n, fs, -20.0, seed=1)
    ref = pink(n, fs, -26.0, seed=2)        # 6 dB quieter: the match must lift it
    analysis = ReferenceAnalysis(ref, fs, {SECTION_WHOLE: (0.0, n / fs)})
    written = []
    for name, state, switches in (
        ("switch-integrated", State(section=SECTION_WHOLE, follow_transport=False), [(2.0, 1), (4.0, 0), (6.0, 1)]),
        ("switch-short", State(section=SECTION_WHOLE, follow_transport=False, match_mode=MATCH_SHORT), [(2.0, 1), (5.0, 0)]),
        ("switch-off", State(section=SECTION_WHOLE, follow_transport=False, match_mode=MATCH_OFF), [(2.0, 1), (5.0, 0)]),
        ("loop-seam", State(section=SECTION_WHOLE, follow_transport=False, listen=LISTEN_REFERENCE,
                            match_mode=MATCH_OFF), []),
    ):
        if name == "loop-seam":
            analysis_2 = ReferenceAnalysis(sine(int(1.5 * fs), fs), fs, {SECTION_WHOLE: (0.0, 1.5)})
            y, info = render_chain(mix[:int(6.0 * fs)], fs, sine(int(1.5 * fs), fs), analysis_2, state,
                                   switch_at=switches)
        else:
            y, info = render_chain(mix, fs, ref, analysis, state, switch_at=switches)
        path = out_dir / ("proto-%s.wav" % name)
        K.write_wav(path, y.astype(np.float32), fs)
        written.append((path, info))
        print("%-20s %-22s peak %6.2f dBFS  LUFS-I %6.2f" % (
            name, info["readout"], K.peak_dbfs(y), K.integrated_lufs(y, fs)))
    return written


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--demo", action="store_true", help="write the G2 reference renders")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "renders"),
                    help="where the renders go")
    ap.add_argument("--sample-rate", type=int, default=48000)
    a = ap.parse_args(argv)
    if a.demo:
        demo(a.out, a.sample_rate)
        return 0
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
