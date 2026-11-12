#!/usr/bin/env python3
"""Re-run the release comparison matrix sequentially with the current binary."""

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
    args = parser.parse_args()
    if min(args.rows, args.scale_rows, args.repeat) <= 0:
        parser.error("row counts and repetitions must be positive")
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or ROOT / "results" / ("reproduction_" + timestamp)).resolve()
    cases = [
        ("sign_b4096", ["--program", "identity", "--threshold", "-1", "--block-rows", "4096"]),
        ("sign_b65536", ["--program", "identity", "--threshold", "-1", "--block-rows", "65536"]),
        ("identity_b64", ["--program", "identity", "--threshold", "0", "--block-rows", "64"]),
        ("energy_b4096", ["--program", "energy", "--threshold", "1000000", "--block-rows", "4096"]),
        ("fir3_b64", ["--program", "fir3", "--threshold", "100000", "--block-rows", "64"]),
        ("negative_energy_b4096", ["--program", str(ROOT / "examples" / "negative_energy.suf"), "--block-rows", "4096"]),
        ("affine_absolute_b4096", ["--program", str(ROOT / "examples" / "affine_absolute.suf"), "--block-rows", "4096"]),
        ("negative_energy_full_refinement", ["--program", str(ROOT / "examples" / "negative_energy.suf"), "--block-rows", "4096", "--patterns", "uniform", "--stages", "32", "--no-specialize"]),
        ("sign_scale_b65536", ["--program", "identity", "--threshold", "-1", "--block-rows", "65536", "--patterns", "uniform"]),
    ]
    for name, options in cases:
        rows = args.scale_rows if name == "sign_scale_b65536" else args.rows
        command = [sys.executable, str(ROOT / "scripts" / "benchmark.py"),
                   "--output", str(output / name), "--rows", str(rows),
                   "--repeat", str(args.repeat)]
        if args.binary:
            command += ["--binary", str(args.binary.resolve())]
        command += options
        print("Running " + name, flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
    print("Complete release matrix: " + str(output))


if __name__ == "__main__":
    main()
