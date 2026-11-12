#!/usr/bin/env python3
"""Reproducible end-to-end benchmark driver; standard library only.

The engine owns algorithm timing and correctness checks. This script records
preparation wall time, the invocation, and the complete engine JSON. It does not
reinterpret requested packed bytes as physical disk, DRAM, or PCIe traffic.
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
PATTERNS = ("clustered", "uniform", "boundary", "smooth")


def atomic_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_command(arguments: list[str], log_path: Path, timeout: float) -> dict:
    start = time.perf_counter()
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log_path.write_text("Command timed out:\n" + json.dumps(arguments) + "\n", encoding="utf-8")
        raise RuntimeError(f"Command exceeded {timeout:g}s; see {log_path}") from error
    elapsed = time.perf_counter() - start
    log_path.write_text(
        "argv: " + json.dumps(arguments) + "\n"
        + f"returncode: {result.returncode}\nwall_seconds: {elapsed:.9f}\n"
        + "stdout:\n" + result.stdout + "\nstderr:\n" + result.stderr,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"Command failed with exit code {result.returncode}; see {log_path}\n"
            + result.stderr[-2000:] + result.stdout[-2000:]
        )
    return {"argv": arguments, "wall_seconds": elapsed, "log": log_path.name}


def lookup(value: dict, *path: str):
    current = value
    for part in path:
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def format_number(value, suffix: str = "") -> str:
    if isinstance(value, int) and not isinstance(value, bool):
        return f"{value:,}{suffix}"
    if isinstance(value, float):
        return f"{value:.6g}{suffix}"
    return "not reported"


def format_ratio(value) -> str:
    return "undefined" if value is None else format_number(value, "×")


def summary_fields(result: dict) -> dict:
    """Explicit engine-schema adapter; raw JSON remains authoritative."""
    return {
        "exact": lookup(result, "verification", "exact_match"),
        "dense_ms": lookup(result, "timings", "dense_fused", "median_ms"),
        "packed_ms": lookup(result, "timings", "packed_full", "median_ms"),
        "progressive_ms": lookup(result, "timings", "progressive", "median_ms"),
        "speedup": result.get("speedup_vs_dense"),
        "dense_bytes": lookup(result, "dense", "requested_payload_bytes"),
        "progressive_bytes": lookup(result, "progressive", "requested_payload_bytes"),
        "payload_reduction": result.get("payload_reduction_vs_dense"),
        "scratch_bytes": lookup(result, "progressive", "peak_scratch_bytes"),
        "refinement_fraction": lookup(result, "progressive", "refinement_fraction"),
    }


def write_summary(path: Path, suite: dict) -> None:
    configuration = suite["configuration"]
    lines = [
        "# Suffice benchmark suite", "",
        f"Status: **{suite['status']}**. Created {suite['created_utc']}.", "",
        f"Platform: {suite['environment']['platform']}", "",
        f"Program: `{configuration['program']}`. Rows per case: {configuration['rows']:,}; "
        f"channels: {configuration['channels']}; stages: `{configuration['stages']}`; "
        f"specialization enabled: {configuration.get('specialization_enabled', True)}.", "",
        "Results apply to this binary, configuration, generated data, and host. "
        "Input payload counters are software requests, not measurements of physical memory traffic.", "",
        "| Case | Exact match | Dense ms | Full packed ms | Progressive ms | Dense / progressive |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for case in suite["cases"]:
        result = case.get("benchmark", {})
        metrics = summary_fields(result)
        exact = "yes" if metrics["exact"] is True else "no" if metrics["exact"] is False else "not reported"
        lines.append(
            f"| {case['pattern']} | {exact} | {format_number(metrics['dense_ms'])} | "
            f"{format_number(metrics['packed_ms'])} | {format_number(metrics['progressive_ms'])} | "
            f"{format_ratio(metrics['speedup'])} |"
        )
    lines.extend([
        "", "## Payload and workspace", "",
        "These are requested encoded payload and runtime scratch counters. "
        "They do not measure physical RAM traffic or total resident process memory.", "",
        "| Case | Dense requested bytes | Progressive requested bytes | Payload reduction | Runtime scratch bytes | Refinement fraction |",
        "| --- | --- | --- | --- | --- | --- |",
    ])
    for case in suite["cases"]:
        metrics = summary_fields(case.get("benchmark", {}))
        refinement = metrics["refinement_fraction"]
        percent = format_number(refinement * 100, "%") if isinstance(refinement, (int, float)) else "not reported"
        lines.append(
            f"| {case['pattern']} | {format_number(metrics['dense_bytes'])} | "
            f"{format_number(metrics['progressive_bytes'])} | {format_ratio(metrics['payload_reduction'])} | "
            f"{format_number(metrics['scratch_bytes'])} | {percent} |"
        )
    lines.extend([
        "", "## Preparation and result files", "",
        "Generation and packing occur before engine timing. These costs are not hidden in a warm-execution speedup.", "",
        "| Case | Generation wall seconds | Packing wall seconds | Engine result |",
        "| --- | --- | --- | --- |",
    ])
    for case in suite["cases"]:
        lines.append(
            f"| {case['pattern']} | {format_number(lookup(case, 'generation', 'wall_seconds'))} | "
            f"{format_number(lookup(case, 'packing', 'wall_seconds'))} | "
            f"[{case['result_file']}]({case['result_file']}) |"
        )
    lines.extend([
        "", "The complete engine output, commands, seed, binary SHA-256, and configuration are retained in [suite.json](suite.json).",
        "The engine result labels whether the dense comparison is a fused builtin or a generic program interpreter.",
        "A custom program must not be described as having beaten a hand-fused baseline unless it was actually compared with one.",
        "A ratio is undefined when its denominator is zero, including constant contracts that request no input payload.",
        "",
    ])
    if "failure" in suite:
        lines.extend(["## Failure", "", suite["failure"], ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("suffice.exe" if os.name == "nt" else "suffice"))
    parser.add_argument("--output", type=Path, help="New output directory; existing per-case files are never overwritten")
    parser.add_argument("--rows", type=int, default=1_048_576)
    parser.add_argument("--channels", type=int, default=1)
    parser.add_argument("--patterns", nargs="+", choices=PATTERNS, default=list(PATTERNS))
    parser.add_argument("--program", default="fir3", help="Builtin name or .suf program path")
    parser.add_argument("--threshold", type=int, help="Override the threshold of a builtin only")
    parser.add_argument("--block-rows", type=int, default=64)
    parser.add_argument("--stages", default="4,8,16,32")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--no-specialize", action="store_true", help="Disable compiled encoded-predicate specializations for an ablation run")
    parser.add_argument("--seed", type=int, default=20261110)
    parser.add_argument("--timeout", type=float, default=600.0, help="Per-command timeout in seconds")
    parser.add_argument("--keep-data", action="store_true", help="Keep generated raw and packed files after a successful suite")
    args = parser.parse_args()
    for name in ("rows", "channels", "block_rows", "repeat", "timeout"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    try:
        stages = [int(item) for item in args.stages.split(",")]
    except ValueError:
        parser.error("--stages must be a comma-separated increasing list of bit counts")
    if not stages or stages[-1] != 32 or any(n < 1 or n > 32 for n in stages) or stages != sorted(set(stages)):
        parser.error("--stages must increase from 1..32 and finish at 32")
    args.stages = ",".join(str(n) for n in stages)
    if len(set(args.patterns)) != len(args.patterns):
        parser.error("--patterns cannot contain duplicates")
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        parser.error(f"Executable does not exist: {args.binary}; build the project first")
    program_path = Path(args.program)
    if program_path.is_file():
        if args.threshold is not None:
            parser.error("A .suf program defines its own threshold; omit --threshold")
        args.program = str(program_path.resolve())
    return args


def main() -> int:
    args = arguments()
    created = dt.datetime.now(dt.timezone.utc)
    output = (args.output or ROOT / "results" / created.strftime("suite_%Y%m%dT%H%M%S_%fZ")).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "suite.json").exists() or (output / "SUMMARY.md").exists():
        print(f"Refusing to overwrite an existing suite in {output}", file=sys.stderr)
        return 2

    suite = {
        "schema_version": 1,
        "status": "running",
        "created_utc": created.isoformat(),
        "environment": {"platform": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(), "python": platform.python_version()},
        "binary": {"path": str(args.binary), "sha256": sha256_file(args.binary)},
        "configuration": {"rows": args.rows, "channels": args.channels, "program": args.program, "threshold": args.threshold, "block_rows": args.block_rows, "stages": args.stages, "repeat": args.repeat, "seed": args.seed, "specialization_enabled": not args.no_specialize},
        "data_retained": bool(args.keep_data),
        "cases": [],
    }
    atomic_json(output / "suite.json", suite)
    generated_paths = []
    try:
        for index, pattern in enumerate(args.patterns):
            print(f"[{index + 1}/{len(args.patterns)}] {pattern}: {args.rows:,} rows", flush=True)
            raw = output / f"{pattern}.i32"
            packed = output / f"{pattern}.sfp"
            result_path = output / f"{pattern}.json"
            if any(path.exists() for path in (raw, packed, result_path)):
                raise FileExistsError(f"A case output already exists for {pattern}; choose a new output directory")
            case = {"pattern": pattern, "seed": args.seed + index, "result_file": result_path.name}
            suite["cases"].append(case)
            generate = [str(args.binary), "generate", "--output", str(raw), "--rows", str(args.rows), "--channels", str(args.channels), "--pattern", pattern, "--seed", str(case["seed"])]
            case["generation"] = run_command(generate, output / f"{pattern}_generate.log", args.timeout)
            generated_paths.append(raw)
            pack = [str(args.binary), "pack", "--input", str(raw), "--output", str(packed), "--channels", str(args.channels), "--block-rows", str(args.block_rows)]
            case["packing"] = run_command(pack, output / f"{pattern}_pack.log", args.timeout)
            generated_paths.append(packed)
            benchmark = [str(args.binary), "bench", "--input", str(raw), "--packed", str(packed), "--channels", str(args.channels), "--program", args.program, "--repeat", str(args.repeat), "--stages", args.stages, "--json", str(result_path)]
            if args.threshold is not None:
                benchmark += ["--threshold", str(args.threshold)]
            if args.no_specialize:
                benchmark.append("--no-specialize")
            case["execution"] = run_command(benchmark, output / f"{pattern}_bench.log", args.timeout)
            case["benchmark"] = json.loads(result_path.read_text(encoding="utf-8"))
            if not isinstance(case["benchmark"], dict):
                raise ValueError(f"Engine result must be a JSON object: {result_path}")
            if lookup(case["benchmark"], "verification", "exact_match") is False:
                raise RuntimeError(f"Exact-output verification failed for {pattern}")
            atomic_json(output / "suite.json", suite)
            write_summary(output / "SUMMARY.md", suite)
        suite["status"] = "completed"
        if not args.keep_data:
            for path in generated_paths:
                path.unlink()
    except (OSError, RuntimeError, ValueError) as error:
        suite["status"] = "failed"
        suite["failure"] = str(error)
        suite["data_retained"] = True
        atomic_json(output / "suite.json", suite)
        write_summary(output / "SUMMARY.md", suite)
        print(str(error), file=sys.stderr)
        print(f"Partial results: {output}", file=sys.stderr)
        return 1
    atomic_json(output / "suite.json", suite)
    write_summary(output / "SUMMARY.md", suite)
    print(f"Results: {output / 'SUMMARY.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
