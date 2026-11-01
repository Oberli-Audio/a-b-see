#!/usr/bin/env python3
"""Render A B see's goldens (D.3.5).

One golden per defining feature that produces audio, with a sidecar naming the flags, the
slice it was rendered on and this command. A golden is **only** regenerated together with a
`Sounds different:` line in CHANGELOG.md (D.5.8): the whole point is that it fails when the
sound changes without anyone meaning to.

    HARNESS=build/ABSeeHarness_artefacts/Release/ABSeeHarness python3 tools/tests/make_golden.py

The test material (seeded pink noise, a 1 kHz sine) is written by the suite's `Material`
class, so a golden never depends on a file in OneDrive.
"""
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "tests"))
sys.path.insert(0, os.environ.get("DSPKIT", str(ROOT / "scripts" / "dsp-kit")))
import dspkit as K  # noqa: E402
import test_plugin as T  # noqa: E402

GOLDENS = {
    # The whole chain in one render: a reference playing at a matched level, the switch moved
    # from the mix to the reference and back, the section loop, trim and the mono sum.
    "switch-and-match": dict(
        params={"matchMode": T.MATCH["integrated"], "section": T.SECTION["whole"],
                "followTransport": 0.0, "trim": 0.5625, "mono": 0.0},
        flags=dict(seed=1, sample_rate=48000, block=512, seconds=3.0, signal="noise:pink:-20",
                   reference="quiet", mix_reading="-27.0",
                   automate="listen:0.0=0.0,0.5=1.0,2.0=0.0"),
        feedback=False),
}


def main():
    harness = os.environ.get("HARNESS", T.HARNESS)
    T.Material.setup()
    out_dir = ROOT / "tools" / "tests" / "golden"
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, spec in GOLDENS.items():
        wav = out_dir / (name + ".wav")
        flags = dict(spec["flags"])
        material = flags["reference"]
        flags["reference"] = str(getattr(T.Material, material))
        flags["out"] = wav
        res = K.render(harness, spec["params"], **flags)
        sidecar = dict(flags=dict(spec["flags"]), params=spec["params"],
                       arch=res["applied"]["build"]["arch"],
                       commit=res["applied"]["build"]["commit"],
                       feedback=spec["feedback"],
                       regenerate="HARNESS=... python3 tools/tests/make_golden.py",
                       note="the `reference` flag names a Material attribute, not a path")
        (out_dir / (name + ".json")).write_text(json.dumps(sidecar, indent=2) + "\n")
        print("%-20s peak %6.2f dBFS  LUFS-I %6.2f  readout %r"
              % (name, res["levels"]["peak_dbfs"], res["loudness"]["integrated_lufs"],
                 res["product"]["match"]["readout"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
