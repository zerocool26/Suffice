#!/usr/bin/env python3
"""Run Suffice 0.2's fixed-versus-selected release matrix sequentially."""

import argparse
import datetime
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rows", type=int, default=1048576)
    parser.add_argument("--scale-rows", type=int, default=16777216)
    parser.add_argument("--repeat", type=int, default=7)
    parser.add_argument("--calibration-repeat", type=int, default=3)
    args = parser.parse_args()
    if min(args.rows, args.scale_rows, args.repeat) <= 0 or not 1 <= args.calibration_repeat <= 9:
        parser.error("counts must be positive and calibration-repeat must be in 1..9")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or ROOT / "results" / ("reproduction_v02_" + stamp)).resolve()
    cases = [
        ("sign_b65536", ["--program", "identity", "--threshold", "-1", "--block-rows", "65536", "--patterns", "clustered", "uniform", "boundary", "smooth"]),
        ("fir3_b64", ["--program", "fir3", "--block-rows", "64", "--patterns", "clustered", "uniform", "boundary", "smooth", "heterogeneous"]),
        ("fir3_packed_b4096", ["--program", "fir3", "--block-rows", "4096", "--packed-only-selection", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("energy_b4096", ["--program", "energy", "--block-rows", "4096", "--patterns", "clustered", "uniform", "boundary"]),
        ("magnitude_two_channels", ["--preset", "magnitude-intersection", "--channels", "2", "--block-rows", "4096", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("difference_two_channels", ["--preset", "cross-channel-difference", "--channels", "2", "--block-rows", "4096", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("positive_two_channels", ["--preset", "positive-intersection", "--channels", "2", "--block-rows", "4096", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("gate_eight_channels", ["--program", str(ROOT / "examples/predicate_eight_channel_gate.suf"), "--channels", "8", "--block-rows", "4096", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("mixed_three_channels", ["--preset", "mixed-three-channels", "--channels", "3", "--block-rows", "4096", "--patterns", "uniform", "boundary", "heterogeneous"]),
        ("sign_scale_b65536", ["--program", "identity", "--threshold", "-1", "--block-rows", "65536", "--patterns", "uniform"]),
    ]
    for name, extra in cases:
        rows = args.scale_rows if name == "sign_scale_b65536" else args.rows
        cmd = [sys.executable, str(ROOT / "scripts/benchmark_v0_2.py"),
               "--output", str(output / name), "--rows", str(rows),
               "--repeat", str(args.repeat), "--calibration-repeat", str(args.calibration_repeat)]
        if args.binary:
            cmd += ["--binary", str(args.binary.resolve())]
        print("Running " + name, flush=True)
        subprocess.run(cmd + extra, cwd=ROOT, check=True)
    print("Completed comparison matrix: " + str(output))


if __name__ == "__main__":
    main()
