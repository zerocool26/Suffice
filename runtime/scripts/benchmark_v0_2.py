#!/usr/bin/env python3
"""Reproduce calibrated selection and independent timing rounds with Suffice 0.2.

Standard library only. Large generated raw/packed inputs are removed after each
successful case unless --keep-data is specified. Complete JSON, commands, program
snapshots, checksums, logs, and preparation costs remain available.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent.parent
PATTERNS = ("clustered", "uniform", "boundary", "smooth", "alternating", "constant", "heterogeneous")
BACKENDS = ("auto", "dense", "raw-vector", "packed-vector", "encoded-predicate", "dense-predicate", "progressive")
PRESETS = {
    "positive-intersection": (2, "input a 0 0\ninput b 1 0\nmin both a b\noutput both > -1\n"),
    "magnitude-intersection": (2, "input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 100000000\n"),
    "mixed-three-channels": (3, "input a 0 0\ninput b 1 0\ninput c 2 0\nabs aa a\nabs bb b\nmin both aa bb\nconst bias 100000000\nsub shifted c bias\nmax either both shifted\noutput either > 200000000\n"),
    "cross-channel-difference": (2, "input a 0 0\ninput b 1 0\nsub difference a b\nabs magnitude difference\noutput magnitude > 100000\n"),
}


def write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def run(arguments: list[str], log: Path, timeout: float) -> dict:
    start = time.perf_counter()
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log.write_text("Timed out: " + json.dumps(arguments) + "\n", encoding="utf-8")
        raise RuntimeError(f"Command exceeded {timeout:g}s; see {log}") from error
    elapsed = time.perf_counter() - start
    log.write_text("argv: " + json.dumps(arguments) + "\n" +
                   f"returncode: {result.returncode}\nwall_seconds: {elapsed:.9f}\n" +
                   "stdout:\n" + result.stdout + "\nstderr:\n" + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}\n" + result.stderr[-2000:])
    return {"argv": arguments, "wall_seconds": elapsed, "log": log.name}


def formatted(value, suffix="") -> str:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return f"{value:.6g}{suffix}"
    return "—"


def summarize(output: Path, suite: dict) -> None:
    cfg = suite["configuration"]
    lines = ["# Suffice 0.2 comparison suite", "", f"Status: **{suite['status']}**.", "",
             f"Created: {suite['created_utc']}. Host: {suite['environment']['platform']}.", "",
             f"Rows: {cfg['rows']:,}; channels: {cfg['channels']}; block rows: {cfg['block_rows']:,}; "
             f"program: `{cfg['program_label']}`; requested backend: `{cfg['backend']}`.", "",
             "The selector is prepared before fresh timing rounds. No calibration sample is reused. "
             "These are independent timing rounds on the same immutable dataset, not unseen test data.", "",
             "The selection universe is **" + ("packed-only" if cfg['packed_only_selection'] else "raw and packed") +
             "**. Fixed raw backends remain comparison baselines in either mode.", "",
             "| Pattern | Selected | Best raw ms | Best packed ms | Selected held-out ms | Selection regret | Exact |",
             "| --- | --- | ---: | ---: | ---: | ---: | --- |"]
    for case in suite["cases"]:
        result = case.get("comparison")
        if not result:
            continue
        raw = result["best"]["raw_fixed"] or {}
        packed = result["best"]["packed_fixed"] or {}
        lines.append(f"| {case['pattern']} | `{result['adaptive']['backend']}` | "
                     f"{formatted(raw.get('median_ms'))} | {formatted(packed.get('median_ms'))} | "
                     f"{formatted(result['adaptive']['timing']['median_ms'])} | "
                     f"{formatted(result['selection_regret']['adaptive_over_best_eligible_fixed'], 'x')} | yes |")
    lines += ["", "Selection regret is selected execution time divided by the fastest eligible fixed "
              "backend's independently measured time. Near one is desirable. Timing noise can put it below one.", "",
              "## Preparation and memory", "",
              "| Pattern | Packing wall ms | Input validation ms | Selector preparation ms | Calibration workspace bytes | Selected scratch bytes |",
              "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for case in suite["cases"]:
        result = case.get("comparison")
        if not result:
            continue
        selection = result["selection"]
        lines.append(f"| {case['pattern']} | {formatted(case['packing']['wall_seconds'] * 1000)} | "
                     f"{formatted(result['preparation']['validation_ms'])} | "
                     f"{formatted(result['preparation']['adaptive_construct_wall_ms'])} | "
                     f"{selection['calibration_workspace_bytes']:,} | {selection['selected_scratch_bytes']:,} |")
    lines += ["", "Packing, validation, compilation, oracle checks, candidate warmup, and calibration are "
              "not hidden inside a warm execution speedup. Forced backend setup costs are retained in each "
              "result's fixed-backend preparation reports. The comparison holds multiple runners at once; "
              "its total retained workspace is separately reported.", "",
              "Requested payload and scratch are software accounting. They are not measured DRAM traffic, "
              "total resident process memory, GPU use, or energy consumption.", "",
              "## Result files", ""]
    for case in suite["cases"]:
        lines.append(f"- `{case['pattern']}`: [{case['result_file']}]({case['result_file']}); "
                     f"seed {case['seed']}; retained data: {case.get('data_retained', True)}.")
    lines += ["", "The complete executable SHA-256, commands, per-case result objects, and configuration "
              "are stored in `suite.json`. Generated or supplied text programs are copied to `program.suf`.", ""]
    if suite.get("failure"):
        lines += ["## Failure", "", suite["failure"], ""]
    (output / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("suffice.exe" if os.name == "nt" else "suffice"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rows", type=int, default=1048576)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--patterns", nargs="+", choices=PATTERNS, default=list(PATTERNS[:4]))
    program = parser.add_mutually_exclusive_group()
    program.add_argument("--program", help="Builtin name or .suf path; defaults to fir3")
    program.add_argument("--preset", choices=PRESETS, help="Write a reproducible multichannel .suf program")
    parser.add_argument("--threshold", type=int, help="Builtin threshold only")
    parser.add_argument("--backend", choices=BACKENDS, default="auto")
    parser.add_argument("--block-rows", type=int, default=4096)
    parser.add_argument("--tile-rows", type=int, default=256)
    parser.add_argument("--max-scratch-mib", type=int, default=128)
    parser.add_argument("--repeat", type=int, default=7)
    parser.add_argument("--calibration-repeat", type=int, default=3)
    parser.add_argument("--stages", default="4,8,16,32")
    parser.add_argument("--no-specialize", action="store_true")
    parser.add_argument("--packed-only-selection", action="store_true")
    parser.add_argument("--seed", type=int, default=20261110)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--keep-data", action="store_true")
    args = parser.parse_args()
    if args.rows <= 0 or not 1 <= args.channels <= 1024:
        parser.error("--rows must be positive and --channels must be in 1..1024")
    if args.block_rows < 32 or args.block_rows > 65536 or args.block_rows & (args.block_rows - 1):
        parser.error("--block-rows must be a power of two in 32..65536")
    if not 1 <= args.tile_rows <= 65536 or args.max_scratch_mib <= 0:
        parser.error("--tile-rows must be in 1..65536 and --max-scratch-mib must be positive")
    if not 1 <= args.repeat <= 1000 or not 1 <= args.calibration_repeat <= 9 or args.timeout <= 0:
        parser.error("repeat must be 1..1000, calibration repeat 1..9, and timeout positive")
    if not 0 <= args.seed <= (1 << 64) - len(args.patterns):
        parser.error("seed and per-pattern increments must fit uint64")
    if len(set(args.patterns)) != len(args.patterns):
        parser.error("duplicate patterns are not permitted")
    try:
        stages = [int(part) for part in args.stages.split(",")]
    except ValueError:
        parser.error("stages must be a comma-separated increasing list")
    if not stages or stages[-1] != 32 or stages != sorted(set(stages)) or any(n < 1 or n > 32 for n in stages):
        parser.error("stages must increase within 1..32 and finish at 32")
    args.stages = ",".join(map(str, stages))
    if args.packed_only_selection and args.backend in ("dense", "raw-vector", "dense-predicate"):
        parser.error("a raw backend cannot be forced with packed-only selection")
    if args.preset and args.channels < PRESETS[args.preset][0]:
        parser.error(f"preset {args.preset} requires at least {PRESETS[args.preset][0]} channels")
    if args.preset and args.threshold is not None:
        parser.error("presets define their own threshold")
    args.program = args.program or "fir3"
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        parser.error("binary does not exist; build Suffice first")
    if not args.preset and args.program not in ("identity", "delta", "fir3", "energy"):
        path = Path(args.program).resolve()
        if not path.is_file():
            parser.error("program file does not exist")
        if args.threshold is not None:
            parser.error("a .suf program defines its own threshold")
        args.program = str(path)
    return args


def main() -> int:
    args = arguments()
    created = dt.datetime.now(dt.timezone.utc)
    output = (args.output or ROOT / "results" / created.strftime("compare_%Y%m%dT%H%M%S_%fZ")).resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print(f"Refusing to reuse existing output directory: {output}", file=sys.stderr)
        return 2
    program = args.program
    label = args.preset or args.program
    if args.preset:
        (output / "program.suf").write_text(PRESETS[args.preset][1], encoding="utf-8")
        program = str(output / "program.suf")
    elif program not in ("identity", "delta", "fir3", "energy"):
        (output / "program.suf").write_bytes(Path(program).read_bytes())
        program = str(output / "program.suf")
    suite = {
        "schema_version": 2, "status": "running", "created_utc": created.isoformat(),
        "environment": {"platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(), "python": platform.python_version()},
        "binary": {"path": str(args.binary), "sha256": digest(args.binary)},
        "configuration": {"rows": args.rows, "channels": args.channels, "program": program, "program_label": label,
                          "threshold": args.threshold, "backend": args.backend, "block_rows": args.block_rows,
                          "tile_rows": args.tile_rows, "max_scratch_mib": args.max_scratch_mib, "repeat": args.repeat,
                          "calibration_repeat": args.calibration_repeat, "stages": args.stages,
                          "progressive_specialization_enabled": not args.no_specialize,
                          "packed_only_selection": args.packed_only_selection, "seed": args.seed},
        "program_sha256": digest(output / "program.suf") if (output / "program.suf").exists() else None,
        "cases": [],
    }
    write_json(output / "suite.json", suite)
    try:
        for index, pattern in enumerate(args.patterns):
            print(f"[{index+1}/{len(args.patterns)}] {pattern}: {args.rows:,} rows, {args.channels} channels", flush=True)
            raw, packed = output / f"{pattern}.i32", output / f"{pattern}.sfp"
            result_path = output / f"{pattern}.json"
            case = {"pattern": pattern, "seed": args.seed + index, "result_file": result_path.name, "data_retained": True}
            suite["cases"].append(case)
            case["generation"] = run([str(args.binary), "generate", "--output", str(raw), "--rows", str(args.rows),
                                      "--channels", str(args.channels), "--pattern", pattern, "--seed", str(case["seed"])],
                                     output / f"{pattern}_generate.log", args.timeout)
            case["packing"] = run([str(args.binary), "pack", "--input", str(raw), "--output", str(packed),
                                   "--channels", str(args.channels), "--block-rows", str(args.block_rows)],
                                  output / f"{pattern}_pack.log", args.timeout)
            command = [str(args.binary), "compare", "--input", str(raw), "--packed", str(packed),
                       "--channels", str(args.channels), "--program", program, "--backend", args.backend,
                       "--repeat", str(args.repeat), "--calibration-repeat", str(args.calibration_repeat),
                       "--tile-rows", str(args.tile_rows), "--max-scratch-mib", str(args.max_scratch_mib),
                       "--stages", args.stages, "--json", str(result_path)]
            if args.threshold is not None:
                command += ["--threshold", str(args.threshold)]
            if args.no_specialize:
                command.append("--no-specialize")
            if args.packed_only_selection:
                command.append("--packed-only-selection")
            case["execution"] = run(command, output / f"{pattern}_compare.log", args.timeout)
            case["comparison"] = json.loads(result_path.read_text(encoding="utf-8"))
            result = case["comparison"]
            if result.get("schema_version") != 2 or result.get("command") != "compare" or result.get("verification", {}).get("exact_match") is not True:
                raise ValueError("missing successful schema-v2 exact comparison result")
            if not args.keep_data:
                raw.unlink(); packed.unlink(); case["data_retained"] = False
            write_json(output / "suite.json", suite)
            summarize(output, suite)
        suite["status"] = "completed"
    except (OSError, ValueError, RuntimeError) as error:
        suite["status"] = "failed"; suite["failure"] = str(error)
        write_json(output / "suite.json", suite); summarize(output, suite)
        print(str(error), file=sys.stderr)
        print(f"Partial results: {output}", file=sys.stderr)
        return 1
    write_json(output / "suite.json", suite); summarize(output, suite)
    print(f"Results: {output / 'SUMMARY.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
