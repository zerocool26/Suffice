#!/usr/bin/env python3
"""Reproduce a focused Suffice 0.4 matrix with immutable same-process controls.

The ordinary distributions use the retained native generator. Threshold-prefix
stress inputs are generated here from a specified SplitMix64 stream, so their
construction is independent of timing observations. Large input files are
removed only after a complete, verified case unless --keep-data is supplied.
"""

from __future__ import annotations

import argparse
import array
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import time

from benchmark_v0_2 import PRESETS, digest, write_json
from benchmark_v0_4 import default_baseline
from codegen_support import ROOT, compiler_config
from reproduce_v0_3 import capture_provenance, run_command


def case_definitions(rows):
    definitions = []

    def add(family, channels, program, patterns, threshold=None, count=None, constant_control=False):
        for pattern in patterns:
            suffix = pattern.removeprefix("spatial_") if family == "magnitude_spatial" else pattern
            definitions.append({"name": family + "_" + suffix, "family": family,
                                "pattern": pattern, "rows": count or rows, "channels": channels,
                                "block_rows": 4096, "program": program, "threshold": threshold,
                                "constant_control": constant_control})

    add("sign", 1, "identity", ["uniform", "smooth"], -1)
    add("positive_two", 2, "preset:positive-intersection", ["uniform", "heterogeneous"])
    add("gate_eight", 8, "examples/predicate_eight_channel_gate.suf", ["uniform", "heterogeneous"])
    add("magnitude_two", 2, "preset:magnitude-intersection", ["uniform", "heterogeneous"])
    add("magnitude_two", 2, "preset:magnitude-intersection", ["prefix_stress"], count=rows+17)
    add("mixed_three", 3, "preset:mixed-three-channels", ["uniform", "heterogeneous", "prefix_stress"])
    add("overlapping_gates", 2, "examples/predicate_overlapping_gates.suf", ["uniform", "heterogeneous", "prefix_stress"])
    add("magnitude_constant", 2, "preset:magnitude-intersection", ["boundary"], constant_control=True)
    add("magnitude_spatial", 2, "spatial:magnitude1000", ["spatial_scattered", "spatial_clustered"])
    return definitions


def prefix_stress(path, rows, channels, family, seed):
    """Values near the exact source thresholds; radius and stream are fixed.

    Magnitude values straddle +/-100,000,000; mixed values straddle
    +/-200,000,000 and +300,000,000. Overlap alternates its two rectangle
    boundaries. These are adversarial to prefix certificates, not claims about
    application data. Output remains variable and is checked by the IR oracle.
    """
    if path.exists():
        raise RuntimeError("refusing to overwrite stress input")
    mask64 = (1 << 64) - 1
    state = seed

    def random64():
        nonlocal state
        state = (state + 0x9E3779B97F4A7C15) & mask64
        value = state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask64
        return value ^ (value >> 31)

    started = time.perf_counter()
    with path.open("wb") as stream:
        for first in range(0, rows, 8192):
            values = array.array("i")
            if values.itemsize != 4:
                raise RuntimeError("stress generator requires a 32-bit array('i')")
            for row in range(first, min(rows, first + 8192)):
                for channel in range(channels):
                    random = random64()
                    delta = int(random % 255) - 127
                    if family == "magnitude_two":
                        center = 100000000 if random & 256 else -100000000
                    elif family == "mixed_three":
                        center = 300000000 if channel == 2 else (200000000 if random & 256 else -200000000)
                    elif family == "overlapping_gates":
                        center = 0 if row % 2 == 0 else (-100000000 if channel == 0 else 100000000)
                    else:
                        raise RuntimeError("unrecognized threshold-prefix stress family")
                    values.append(center + delta)
            if sys.byteorder != "little":
                values.byteswap()
            values.tofile(stream)
    return {"generator": "threshold_prefix_splitmix64_v1", "seed": seed, "radius": 127,
            "family": family, "rows": rows, "channels": channels,
            "process_wall_ms": (time.perf_counter() - started) * 1000,
            "output_bytes": path.stat().st_size}


def spatial_stress(path, rows, pattern):
    """Identical row multiset; only spatial placement of uncertainty differs."""
    if path.exists() or rows < 512 or rows % 512:
        raise RuntimeError("spatial pair needs new output and rows divisible by 512 (at least 512)")
    uncertain_rows = rows // 256
    counts = {"1000": uncertain_rows // 2, "1001": uncertain_rows // 2,
              "1000000000": rows - uncertain_rows}
    fingerprint = hashlib.sha256(json.dumps({"channels": 2, "counts_per_channel": counts}, sort_keys=True).encode()).hexdigest()
    started = time.perf_counter()
    with path.open("wb") as stream:
        for first in range(0, rows, 8192):
            values = array.array("i")
            if values.itemsize != 4:
                raise RuntimeError("spatial generator needs 32-bit array('i')")
            for row in range(first, min(rows, first + 8192)):
                if pattern == "spatial_scattered":
                    uncertain = row % 256 == 0
                    uncertainty_index = row // 256
                elif pattern == "spatial_clustered":
                    uncertain = row < uncertain_rows
                    uncertainty_index = row
                else:
                    raise RuntimeError("invalid spatial stress pattern")
                value = 1000 + uncertainty_index % 2 if uncertain else 1000000000
                values.extend((value, value))
            if sys.byteorder != "little":
                values.byteswap()
            values.tofile(stream)
    return {"generator": "spatial_uncertainty_pair_v1", "pattern": pattern, "rows": rows, "channels": 2,
            "uncertain_rows": uncertain_rows, "counts_per_channel": counts,
            "row_multiset_fingerprint": fingerprint, "identical_channels_per_row": True,
            "process_wall_ms": (time.perf_counter() - started) * 1000,
            "output_bytes": path.stat().st_size}


def identities(binary, baseline):
    return {"current": {"path": str(binary), "sha256": digest(binary)},
            "baseline": {"path": str(baseline), "sha256": digest(baseline)}}


def verify_sources(binary, baseline, binaries, provenance):
    if identities(binary, baseline) != binaries:
        raise RuntimeError("A current or baseline executable changed during the matrix")
    current = capture_provenance(True)
    if current != provenance:
        changed = sorted(key for key in set(current["inputs"]) | set(provenance["inputs"])
                         if current["inputs"].get(key) != provenance["inputs"].get(key))
        raise RuntimeError("Source/compiler identity changed during matrix: " + ", ".join(changed[:8]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("suffice.exe" if os.name == "nt" else "suffice"))
    parser.add_argument("--baseline-binary", type=Path, default=default_baseline())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=1048576)
    parser.add_argument("--repeat", type=int, default=40, help="Native minimum rounds, rounded to complete cycles")
    parser.add_argument("--core-repeat", type=int, default=36, help="Fresh sampled-selector/core rounds; 36 is two cycles for nine candidates")
    parser.add_argument("--seed", type=int, default=20261111)
    parser.add_argument("--cases", nargs="+", help="Exact names or family names; omit for all 18 cases")
    parser.add_argument("--pin-cpu", default="auto")
    parser.add_argument("--skip-core", action="store_true")
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.rows < 1 or not 1 <= args.repeat <= 1000 or not 1 <= args.core_repeat <= 1000:
        parser.error("invalid rows or repetition count")
    if not 0 <= args.seed <= (1 << 64) - 1:
        parser.error("seed must fit uint64")
    binary, baseline = args.binary.resolve(), args.baseline_binary.resolve()
    if not binary.is_file() or not baseline.is_file():
        parser.error("build the current and 0.3 baseline executables first")
    available = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    if args.pin_cpu != "none" and available:
        cpu = available[0] if args.pin_cpu == "auto" else int(args.pin_cpu)
        if cpu not in available:
            parser.error("CPU is outside allowed affinity")
        os.sched_setaffinity(0, {cpu})
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    definitions = case_definitions(args.rows)
    if args.cases:
        requested = set(args.cases)
        definitions = [case for case in definitions if case["name"] in requested or case["family"] in requested]
        if not definitions:
            parser.error("no requested case exists")
    if any(case["family"] == "magnitude_spatial" for case in definitions) and (args.rows < 512 or args.rows % 512):
        parser.error("the selected spatial pair requires --rows divisible by 512 and at least 512")
    output = args.output.resolve()
    configuration = {"rows": args.rows, "minimum_repeat": args.repeat, "core_repeat": args.core_repeat,
                     "seed": args.seed, "skip_core": args.skip_core, "definitions": definitions,
                     "affinity": affinity, "prefix_depths": [8, 12, 16], "portable_prefix_depth": 12}
    provenance = capture_provenance(True)
    binaries = identities(binary, baseline)
    suite_path = output / "suite.json"
    if output.exists():
        if not args.resume or not suite_path.is_file():
            parser.error("use a fresh output directory or an explicit valid --resume")
        suite = json.loads(suite_path.read_text(encoding="utf-8"))
        if suite["configuration"] != configuration or suite["binaries"] != binaries or suite["provenance"] != provenance:
            parser.error("resume refused: configuration, source, compiler or binary identity changed")
    else:
        output.mkdir(parents=True)
        suite = {"schema_version": 4, "status": "running", "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 "configuration": configuration, "binaries": binaries, "provenance": provenance,
                 "environment": {"platform": platform.platform(), "python": platform.python_version(),
                                 "machine": platform.machine(), "allowed_affinity": available,
                                 "benchmark_affinity": affinity}, "cases": []}
    write_json(suite_path, suite)
    completed = {case["name"] for case in suite["cases"] if case.get("status") == "completed"}
    try:
        for number, definition in enumerate(definitions, 1):
            verify_sources(binary, baseline, binaries, provenance)
            name = definition["name"]
            if name in completed:
                print(f"[{number}/{len(definitions)}] {name}: retained complete case", flush=True)
                continue
            directory = output / name
            if directory.exists():
                raise RuntimeError("Incomplete case needs explicit inspection before resume: " + str(directory))
            directory.mkdir()
            case = {**definition, "seed": args.seed, "status": "running"}
            suite["cases"].append(case)
            write_json(suite_path, suite)
            print(f"[{number}/{len(definitions)}] {name}: generate/pack", flush=True)
            raw, packed = directory / "input.i32", directory / "input.sfp"
            if definition["pattern"].startswith("spatial_"):
                case["generation"] = spatial_stress(raw, definition["rows"], definition["pattern"])
                write_json(directory / "generate.json", case["generation"])
            elif definition["pattern"] == "prefix_stress":
                case["generation"] = prefix_stress(raw, definition["rows"], definition["channels"], definition["family"], args.seed)
                write_json(directory / "generate.json", case["generation"])
            else:
                case["generation"] = run_command([binary, "generate", "--output", raw, "--rows", definition["rows"],
                                                   "--channels", definition["channels"], "--pattern", definition["pattern"],
                                                   "--seed", args.seed], directory / "generate.log")
            case["packing"] = run_command([binary, "pack", "--input", raw, "--output", packed,
                                            "--channels", definition["channels"], "--block-rows", definition["block_rows"]], directory / "pack.log")
            case["raw_sha256"], case["packed_sha256"] = digest(raw), digest(packed)
            program = definition["program"]
            if program == "spatial:magnitude1000":
                program_path = directory / "program.suf"
                program_path.write_text("input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 1000\n", encoding="utf-8")
                program = str(program_path)
            elif program.startswith("preset:"):
                program_path = directory / "program.suf"
                program_path.write_text(PRESETS[program.split(":", 1)[1]][1], encoding="utf-8")
                program = str(program_path)
            elif program.endswith(".suf"):
                program_path = directory / "program.suf"
                shutil.copy2(ROOT / program, program_path)
                program = str(program_path)
            case["program_used"] = program
            case["program_sha256"] = digest(Path(program)) if Path(program).is_file() else None
            if not args.skip_core:
                print("  sampled selector and core controls", flush=True)
                command = [binary, "compare", "--input", raw, "--packed", packed, "--program", program,
                           "--repeat", args.core_repeat, "--calibration", "sampled", "--calibration-repeat", 3,
                           "--sample-seed", args.seed, "--json", directory / "core.json"]
                if definition["threshold"] is not None:
                    command += ["--threshold", definition["threshold"]]
                case["core_execution"] = run_command(command, directory / "core.log")
                case["core"] = json.loads((directory / "core.json").read_text(encoding="utf-8"))
                if not case["core"].get("verification", {}).get("exact_match"):
                    raise RuntimeError("core comparison lacks exact complete-output validation")
                selection = case["core"]["selection"]
                if selection["total_calibration_row_evaluations"] > selection["calibration_row_budget"]:
                    raise RuntimeError("sampled selector exceeded its requested-row budget")
                write_json(suite_path, suite)
            print("  same-process frozen/current kernels and raw AoS/SoA controls", flush=True)
            native = directory / "native"
            command = [sys.executable, ROOT / "scripts/benchmark_v0_4.py", "--binary", binary,
                       "--baseline-binary", baseline, "--raw", raw, "--packed", packed,
                       "--program", program, "--output", native, "--repeat", args.repeat,
                       "--compiler", provenance["native_compiler"]["executable"], "--pin-cpu", "none"]
            if definition["threshold"] is not None:
                command += ["--threshold", definition["threshold"]]
            case["native_execution"] = run_command(command, directory / "native.log", timeout=1800)
            case["native_result"] = json.loads((native / "result.json").read_text(encoding="utf-8"))
            result = case["native_result"]
            if not result["measurements"]["exact"] or result["compiler"] != provenance["native_compiler"]:
                raise RuntimeError("native exactness or compiler identity mismatch")
            if case.get("core"):
                if case["core"]["verification"]["mask_checksum"] != result["measurements"]["mask_checksum"]:
                    raise RuntimeError("core/native complete output checksums disagree")
                if case["core"]["verification"]["accepted"] != result["measurements"]["accepted"]:
                    raise RuntimeError("core/native accepted counts disagree")
            verify_sources(binary, baseline, binaries, provenance)
            case["data_retained"] = args.keep_data
            if not args.keep_data:
                raw.unlink()
                packed.unlink()
            case["status"] = "completed"
            write_json(directory / "case.json", case)
            write_json(suite_path, suite)
        suite["status"] = "completed"
        suite["completed_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    except Exception as error:
        suite["status"] = "failed"
        suite["failure"] = str(error)
        write_json(suite_path, suite)
        raise
    write_json(suite_path, suite)
    print(json.dumps({"status": suite["status"], "cases": len(suite["cases"]), "matrix": str(output)}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
