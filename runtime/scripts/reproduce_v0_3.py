#!/usr/bin/env python3
"""Paired full/sampled preparation and fused raw/encoded release experiments.

Standard library only. Commands are argument arrays. Each case retains seeds,
source/binary/data identities, exact result JSON, logs, and generated kernels.
Large generated input files are removed only after a successful complete case.
"""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

from benchmark_v0_2 import PRESETS, digest, write_json
from codegen_support import compiler_config

ROOT = Path(__file__).resolve().parent.parent


def capture_provenance(native_enabled):
    """Identity of executable inputs, excluding results, docs, and bytecode."""
    patterns = {"src": ("*.cpp",), "include": ("*.hpp",),
                "scripts": ("*.py", "*.sh", "*.ps1"), "examples": ("*.suf",)}
    paths = set()
    for directory, globs in patterns.items():
        for pattern in globs:
            paths.update(path for path in (ROOT / directory).rglob(pattern) if path.is_file())
    sources = {str(path.relative_to(ROOT)).replace(os.sep, "/"): digest(path)
               for path in sorted(paths)}
    return {"inputs": sources, "native_compiler": compiler_config() if native_enabled else None}


def verify_provenance(binary, binary_identity, expected, native_enabled):
    if digest(binary) != binary_identity["sha256"]:
        raise RuntimeError("Executable changed during the release matrix; use a fresh output directory")
    current = capture_provenance(native_enabled)
    if current != expected:
        changed = sorted(name for name in set(current["inputs"]) | set(expected["inputs"])
                         if current["inputs"].get(name) != expected["inputs"].get(name))
        details = ", ".join(changed[:8])
        if current["native_compiler"] != expected["native_compiler"]:
            details = (details + "; " if details else "") + "native compiler/configuration"
        raise RuntimeError("Source or compiler provenance changed during the release matrix: " + details)


def run_command(argv, log, timeout=900):
    argv = list(map(str, argv))
    started = time.perf_counter()
    usage = None
    with log.open("w", encoding="utf-8") as stream:
        stream.write("argv: " + json.dumps(argv) + "\n")
        stream.flush()
        process = subprocess.Popen(argv, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
        if sys.platform.startswith("linux") and hasattr(os, "wait4"):
            while True:
                pid, status, metrics = os.wait4(process.pid, os.WNOHANG)
                if pid:
                    process.returncode = os.waitstatus_to_exitcode(status)
                    usage = {"peak_rss_kib": metrics.ru_maxrss, "user_cpu_seconds": metrics.ru_utime,
                             "system_cpu_seconds": metrics.ru_stime,
                             "minor_page_faults": metrics.ru_minflt, "major_page_faults": metrics.ru_majflt,
                             "voluntary_context_switches": metrics.ru_nvcsw,
                             "involuntary_context_switches": metrics.ru_nivcsw}
                    break
                if time.perf_counter() - started > timeout:
                    process.kill()
                    _, status, _ = os.wait4(process.pid, 0)
                    process.returncode = os.waitstatus_to_exitcode(status)
                    raise RuntimeError(f"Command timed out; see {log}")
                time.sleep(0.05)
        else:
            process.wait(timeout=timeout)
        elapsed = (time.perf_counter() - started) * 1000
        stream.write(f"\nreturncode: {process.returncode}\nprocess_wall_ms: {elapsed:.6f}\n")
    if process.returncode:
        raise RuntimeError(f"Command failed ({process.returncode}); see {log}")
    return {"argv": argv, "process_wall_ms": elapsed, "log": log.name, "process_usage": usage}


def case_definitions(rows, scale_rows):
    definitions = []
    def add(name, channels, program, patterns, block=4096, threshold=None, packed_only=False, count=None, native=True):
        for pattern in patterns:
            definitions.append({"name": name + "_" + pattern, "family": name, "pattern": pattern,
                                "channels": channels, "program": program, "rows": count or rows,
                                "block_rows": block, "threshold": threshold,
                                "packed_only_selection": packed_only, "native": native})
    add("sign", 1, "identity", ["uniform", "smooth"], 65536, -1)
    add("positive_two", 2, "preset:positive-intersection", ["uniform", "boundary", "heterogeneous"])
    add("gate_eight", 8, "examples/predicate_eight_channel_gate.suf", ["uniform", "boundary", "heterogeneous"])
    add("magnitude_two", 2, "preset:magnitude-intersection", ["uniform", "boundary", "heterogeneous"])
    add("mixed_three", 3, "preset:mixed-three-channels", ["uniform", "heterogeneous"])
    add("difference_two", 2, "preset:cross-channel-difference", ["uniform", "heterogeneous"])
    add("fir3", 1, "fir3", ["uniform", "heterogeneous"], threshold=100000)
    add("fir3_packed_only", 1, "fir3", ["uniform", "heterogeneous"], threshold=100000, packed_only=True, native=False)
    add("overlapping_gates", 2, "examples/predicate_overlapping_gates.suf", ["uniform", "heterogeneous"])
    add("sign_scale", 1, "identity", ["uniform"], 65536, -1, count=scale_rows)
    return definitions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("suffice.exe" if os.name == "nt" else "suffice"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rows", type=int, default=1048576)
    parser.add_argument("--scale-rows", type=int, default=16777216)
    parser.add_argument("--repeat", type=int, default=40)
    parser.add_argument("--calibration-repeat", type=int, default=3)
    parser.add_argument("--sample-rows", type=int, default=32768)
    parser.add_argument("--sample-windows", type=int, default=8)
    parser.add_argument("--work-rows", type=int, default=1048576)
    parser.add_argument("--calibration-ms", type=int, default=25)
    parser.add_argument("--seed", type=int, default=20261110)
    parser.add_argument("--cases", nargs="+", help="Exact case names or family names; omit for the full matrix")
    parser.add_argument("--pin-cpu", default="auto", help="auto, none, or an allowed CPU number; Linux only")
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--resume", action="store_true", help="Resume only when binary, source, compiler, and configuration identities match")
    args = parser.parse_args()
    if min(args.rows, args.scale_rows, args.sample_rows, args.work_rows) <= 0 or not 1 <= args.repeat <= 1000 or not 1 <= args.calibration_repeat <= 9 or not 1 <= args.sample_windows <= 64 or args.calibration_ms < 0:
        parser.error("invalid positive counts, repetition range, or calibration budget")
    binary = args.binary.resolve()
    if not binary.is_file(): parser.error("build the executable first")
    available_affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    if args.pin_cpu != "none" and available_affinity:
        cpu = available_affinity[0] if args.pin_cpu == "auto" else int(args.pin_cpu)
        if cpu not in available_affinity: parser.error("requested CPU is not in the allowed affinity set")
        os.sched_setaffinity(0, {cpu})
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    definitions = case_definitions(args.rows, args.scale_rows)
    if args.cases:
        names = set(args.cases)
        definitions = [d for d in definitions if d["name"] in names or d["family"] in names]
        if not definitions: parser.error("no selected case exists")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or ROOT / "results" / ("release_v0_3_" + stamp)).resolve()
    configuration = {"rows": args.rows, "scale_rows": args.scale_rows, "repeat": args.repeat,
                     "calibration_repeat": args.calibration_repeat, "sample_rows": args.sample_rows,
                     "sample_windows": args.sample_windows, "work_rows": args.work_rows,
                     "calibration_ms": args.calibration_ms, "seed": args.seed,
                     "skip_native": args.skip_native, "definitions": definitions, "affinity": affinity}
    binary_identity = {"path": str(binary), "sha256": digest(binary)}
    native_enabled = not args.skip_native and any(d["native"] for d in definitions)
    provenance = capture_provenance(native_enabled)
    summary_path = output / "suite.json"
    if output.exists():
        if not args.resume or not summary_path.exists(): parser.error("use a fresh output directory or an explicit valid --resume")
        suite = json.loads(summary_path.read_text(encoding="utf-8"))
        if suite["configuration"] != configuration or suite["binary"] != binary_identity:
            parser.error("resume refused: binary or configuration changed")
        if suite.get("provenance") != provenance:
            parser.error("resume refused: source or native compiler provenance changed or is missing")
    else:
        output.mkdir(parents=True)
        suite = {"schema_version": 3, "status": "running", "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 "binary": binary_identity, "configuration": configuration, "provenance": provenance,
                 "environment": {"platform": platform.platform(), "machine": platform.machine(),
                                 "logical_cpus": os.cpu_count(), "allowed_affinity": available_affinity,
                                 "benchmark_affinity": affinity, "python": platform.python_version()}, "cases": []}
    write_json(summary_path, suite)
    completed = {c["name"] for c in suite["cases"] if c.get("status") == "completed"}
    try:
        for index, definition in enumerate(definitions):
            verify_provenance(binary, binary_identity, provenance, native_enabled)
            name = definition["name"]
            if name in completed:
                print("Already complete: " + name, flush=True)
                continue
            directory = output / name
            if directory.exists(): raise RuntimeError("Incomplete case directory requires inspection before resume: " + str(directory))
            directory.mkdir()
            case = {**definition, "seed": args.seed, "status": "running"}
            suite["cases"].append(case)
            write_json(summary_path, suite)
            print(f"[{index+1}/{len(definitions)}] {name}: generate and pack", flush=True)
            raw, packed = directory / "input.i32", directory / "input.sfp"
            case["generation"] = run_command([binary, "generate", "--output", raw, "--rows", definition["rows"],
                                               "--channels", definition["channels"], "--pattern", definition["pattern"],
                                               "--seed", args.seed], directory / "generate.log")
            case["packing"] = run_command([binary, "pack", "--input", raw, "--output", packed,
                                            "--channels", definition["channels"], "--block-rows", definition["block_rows"]], directory / "pack.log")
            case["raw_sha256"], case["packed_sha256"] = digest(raw), digest(packed)
            program = definition["program"]
            if program.startswith("preset:"):
                program_path = directory / "program.suf"
                program_path.write_text(PRESETS[program.split(":", 1)[1]][1], encoding="utf-8")
                program = str(program_path)
            elif program.endswith(".suf"):
                program_path = directory / "program.suf"
                shutil.copy2(ROOT / program, program_path)
                program = str(program_path)
            case["program_used"] = program
            case["program_sha256"] = digest(Path(program)) if Path(program).is_file() else None
            # Alternate which preparation mode runs first across cases.
            modes = ["sampled", "full"] if index % 2 == 0 else ["full", "sampled"]
            case["mode_order"] = modes
            for mode in modes:
                result = directory / (mode + ".json")
                command = [binary, "compare", "--input", raw, "--packed", packed, "--program", program,
                           "--repeat", args.repeat, "--calibration-repeat", args.calibration_repeat,
                           "--calibration", mode, "--sample-rows", args.sample_rows,
                           "--sample-windows", args.sample_windows, "--sample-seed", args.seed,
                           "--calibration-work-rows", args.work_rows, "--calibration-ms", args.calibration_ms,
                           "--json", result]
                if definition["threshold"] is not None: command += ["--threshold", definition["threshold"]]
                if definition["packed_only_selection"]: command += ["--packed-only-selection"]
                print("  " + mode + " preparation and fresh fixed comparison", flush=True)
                case[mode + "_execution"] = run_command(command, directory / (mode + ".log"))
                comparison = json.loads(result.read_text(encoding="utf-8"))
                if comparison.get("schema_version") != 3 or not comparison["verification"]["exact_match"]:
                    raise RuntimeError("Missing schema3 exact comparison")
                if mode == "sampled" and comparison["selection"]["total_calibration_row_evaluations"] > args.work_rows:
                    raise RuntimeError("Measured sampled preparation exceeded requested-row budget")
                case[mode] = comparison
                write_json(summary_path, suite)
            if definition["native"] and not args.skip_native:
                print("  emitted fused native/encoded comparison", flush=True)
                native = directory / "native"
                command = [sys.executable, ROOT / "scripts/benchmark_codegen.py", "--binary", binary,
                           "--raw", raw, "--packed", packed, "--program", program,
                           "--output", native, "--repeat", args.repeat,
                           "--compiler", provenance["native_compiler"]["executable"]]
                if definition["threshold"] is not None: command += ["--threshold", definition["threshold"]]
                case["native_execution"] = run_command(command, directory / "native.log")
                case["native_result"] = json.loads((native / "result.json").read_text(encoding="utf-8"))
                if not case["native_result"]["measurements"]["exact"]:
                    raise RuntimeError("Generated-kernel exactness failed")
                if case["native_result"]["compiler"] != provenance["native_compiler"]:
                    raise RuntimeError("Native benchmark used a different compiler/configuration")
            # Also catch changes during the final case before committing or
            # deleting its reproducible inputs; the next-case check is not enough.
            verify_provenance(binary, binary_identity, provenance, native_enabled)
            case["data_retained"] = args.keep_data
            if not args.keep_data:
                raw.unlink()
                packed.unlink()
            case["status"] = "completed"
            write_json(directory / "case.json", case)
            write_json(summary_path, suite)
        suite["status"] = "completed"
        suite["completed_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    except Exception as error:
        suite["status"] = "failed"
        suite["failure"] = str(error)
        write_json(summary_path, suite)
        raise
    write_json(summary_path, suite)
    print("Completed release matrix: " + str(output), flush=True)


if __name__ == "__main__":
    main()
