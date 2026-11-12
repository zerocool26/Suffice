#!/usr/bin/env python3
"""Audit a relocated full Suffice0.11 session-evidence archive, without execution.

Uses only Python's standard library. Historical absolute source/tool paths are
provenance strings, never read. Included exact corpus copies and frozen measured
sources are the content anchors. This checks recorded comparison descriptors,
not a new native rerun of every historical output. Requires the full evidence
distribution; the compact source-only distribution omits required inputs.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import statistics
import struct
import sys

PROGRAM = "input byte_length 0 0\noutput byte_length > 1024\n"
TRACES = {"reuse": list(range(12)) * 4,
          "pressure": [0, 1] * 4 + list(range(2, 12)) + [0, 1] * 4}
TIMES = ("service_ms", "raw_session_wall_ms", "oracle_validation_ms", "startup_ready_ms",
         "admission_request_ms", "query_request_ms", "stats_request_ms", "close_ms",
         "native_plan_ms", "native_execution_ms", "native_cache_build_ms")
RATIOS = ("service_ms", "query_request_ms", "native_query_ms", "raw_session_wall_ms")
SHA = re.compile(r"[0-9a-f]{64}\Z")


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read(path, cap=32 << 20):
    path = Path(path)
    check(path.is_file() and not path.is_symlink(), f"missing/nonregular/symlink file: {path}")
    check(path.stat().st_size <= cap, f"audit file cap exceeded: {path}")
    with path.open("rb") as stream:
        value = stream.read(cap + 1)
    check(len(value) <= cap, "audit input grew beyond cap")
    return value


def json_file(path):
    return json.loads(read(path).decode("utf-8"))


def local(root, relative):
    check(isinstance(relative, str) and "\\" not in relative and "\0" not in relative,
          "invalid archive-relative path")
    pieces = PurePosixPath(relative)
    check(not pieces.is_absolute() and all(p not in ("", ".", "..") for p in pieces.parts) and bool(pieces.parts),
          "unsafe archive-relative path")
    path = Path(root).joinpath(*pieces.parts)
    check(path.resolve().is_relative_to(Path(root).resolve()), "archive path escapes root")
    return path


def inventory(root):
    result = {}
    for path in sorted(Path(root).rglob("*")):
        check(not path.is_symlink(), "symlink in evidence inventory")
        if path.is_file():
            check(len(result) < 10000, "evidence inventory count cap exceeded")
            data = read(path)
            result[path.relative_to(root).as_posix()] = {"bytes": len(data), "sha256": digest(data)}
    return result


def same(actual, expected, label):
    if isinstance(expected, float):
        check(isinstance(actual, (float, int)) and math.isfinite(actual) and
              math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-8), label)
    elif isinstance(expected, dict):
        check(isinstance(actual, dict) and actual.keys() == expected.keys(), label + " keys")
        for key, value in expected.items():
            same(actual[key], value, label + "." + str(key))
    elif isinstance(expected, list):
        check(isinstance(actual, list) and len(actual) == len(expected), label + " length")
        for index, value in enumerate(expected):
            same(actual[index], value, label + f"[{index}]")
    else:
        check(actual == expected, label)


def describe(values):
    check(values and all(math.isfinite(x) and x > 0 for x in values), "nonpositive/nonfinite ratio")
    return {"values": values, "median": statistics.median(values), "minimum": min(values),
            "maximum": max(values), "geometric_mean": math.exp(sum(map(math.log, values)) / len(values))}


def relative_historical(path, historic_root):
    # Historic producer ran on Linux. PurePosixPath works unchanged on Windows.
    return PurePosixPath(path).relative_to(PurePosixPath(historic_root)).as_posix()


def audit(project_root, evidence, require_binaries=False):
    project_root, evidence = Path(project_root).resolve(), Path(evidence).resolve()
    suite_data = read(evidence / "suite.json")
    suite = json.loads(suite_data)
    run_data = read(evidence / "RUN.json")
    run = json.loads(run_data)
    summary = json_file(evidence / "SUMMARY.json")
    check(suite["status"] == "prepared" and run["status"] == "passed" and summary["status"] == "passed",
          "preparation/run/summary is not passed")
    check(suite["project_count"] == 12 and suite["program"] == PROGRAM and suite["trace_projects"] == TRACES,
          "unexpected experiment program/trace contract")
    pairs = run["pairs_per_trace"]
    check(type(pairs) is int and 1 <= pairs <= 12, "invalid pair count")
    same(run["suite_sha256"], digest(suite_data), "suite identity")
    same(summary["suite_sha256"], digest(suite_data), "summary suite identity")
    same(summary["run_sha256"], digest(run_data), "summary run identity")
    same(run["code_before"], suite["code_identity"], "initial code identity")
    same(run["code_after"], suite["code_identity"], "final code identity")
    same(run["inputs_before"], suite["input_identity"], "initial input identity")
    same(run["inputs_after"], suite["input_identity"], "final input identity")

    actual_inputs = {"inputs/" + name: value for name, value in inventory(evidence / "inputs").items()}
    same(actual_inputs, suite["input_identity"], "included input inventory and bytes")
    frozen = inventory(evidence / "measurement_source")
    check(set(frozen) == set(suite["code_identity"]["project"]), "frozen source inventory differs")
    for name, expected in suite["code_identity"]["project"].items():
        check(frozen[name]["sha256"] == expected, "frozen measured source bytes differ: " + name)

    # Never access suite.source_root or another recorded absolute external path.
    all_members = suite["source_files"]
    check(len(all_members) <= 10000 and all_members, "invalid original-file inventory")
    check([x["source_relative"] for x in all_members] == sorted(
        (x["source_relative"] for x in all_members), key=lambda name: name.encode("utf-8")), "corpus partition order")
    seen = set()
    by_project = {p: [] for p in range(12)}
    for index, item in enumerate(all_members):
        name, project = item["source_relative"], item["project"]
        check(project == index % 12 and name not in seen, "corpus modulo partition/uniqueness")
        seen.add(name)
        check(PurePosixPath(name).parts[0] in suite["source_directories"] and
              PurePosixPath(name).suffix in suite["source_suffixes"], "corpus file scope")
        expected_copy = f"inputs/projects/project_{project:02d}/{name}"
        check(item["copied_relative"] == expected_copy, "corpus copy location")
        data = read(local(evidence, expected_copy), 1 << 20)
        check(len(data) == item["bytes"] and digest(data) == item["sha256"], "included original-copy identity")
        by_project[project].append({"path": name, "data": data, "bytes": len(data), "sha256": digest(data)})
    corpus = {"files": len(all_members), "plaintext_bytes": sum(x["bytes"] for x in all_members),
              "selected_files": sum(x["bytes"] > 1024 for x in all_members),
              "selected_bytes": sum(x["bytes"] for x in all_members if x["bytes"] > 1024)}
    same(suite["corpus"], corpus, "prepared corpus totals")
    same(summary["corpus"], corpus, "summary corpus totals")
    check(len(suite["projects"]) == 12, "generation inventory")
    descriptors, capacities = {}, {}
    logical_snapshot_bytes = 0
    for project in range(12):
        record = suite["projects"][project]
        members = sorted(by_project[project], key=lambda x: x["path"].encode("utf-8"))
        descriptors[project] = [{"row": i, "path": x["path"], "bytes": x["bytes"], "sha256": x["sha256"]}
                                for i, x in enumerate(members) if x["bytes"] > 1024]
        expected_bytes = sum(x["bytes"] for x in descriptors[project])
        capacities[project] = expected_bytes + len(descriptors[project]) * 24
        check(record["project"] == project and record["name"] == f"project_{project:02d}" and
              record["rows"] == len(members) and record["selected_rows"] == len(descriptors[project]) and
              record["selected_bytes"] == expected_bytes and record["expected_result_capacity"] == capacities[project] and
              record["plaintext_bytes"] == sum(x["bytes"] for x in members), "project dimensions")
        catalog = local(evidence, record["catalog_directory"])
        generation = record["generation"]
        check(re.fullmatch(r"[0-9a-f]{32}", generation) is not None, "generation label")
        check(read(catalog / "CURRENT") == (generation + "\n").encode("ascii"), "CURRENT identity")
        directory = catalog / "generations" / generation
        manifest = read(directory / "manifest.txt", 16 << 10)
        check(digest(manifest) == record["manifest_sha256"], "generation manifest identity")
        lines = manifest.decode("ascii").splitlines()
        check(manifest.endswith(b"\n") and b"\r" not in manifest and len(lines) == 9 and
              lines[:5] == ["SUFCAT01", "generation\t" + generation, "rows\t" + str(len(members)), "channels\t4", "schema\t1"],
              "generation schema")
        components = {}
        for line, name in zip(lines[5:], ("features.i32", "features.sfp", "payload.spp", "rows.tsv")):
            fields = line.split("\t")
            check(len(fields) == 4 and fields[:2] == ["component", name] and SHA.fullmatch(fields[3]), "component manifest")
            data = read(directory / name)
            check(fields[2] == str(len(data)) and fields[3] == digest(data), "generation component identity")
            components[name] = data
        metadata = b"".join(f"{i}\t{x['path'].encode('utf-8').hex()}\t{x['bytes']}\t{x['sha256']}\n".encode("ascii")
                             for i, x in enumerate(members))
        check(components["rows.tsv"] == metadata, "generation paths/sizes/digests differ from included originals")
        feature_rows = []
        for member in members:
            suffix = PurePosixPath(member["path"]).suffix.lower()
            extension = 2 if suffix in {".cpp", ".hpp"} else 4 if suffix == ".py" else 0
            try:
                member["data"].decode("utf-8", "strict")
                utf8 = int(b"\0" not in member["data"])
            except UnicodeDecodeError:
                utf8 = 0
            feature_rows.append(struct.pack("<4i", member["bytes"], extension, member["data"].count(b"\n"), utf8))
        check(components["features.i32"] == b"".join(feature_rows), "raw derived features differ from independent source-byte extraction")
        packed = components["features.sfp"]
        check(len(packed) >= 64, "short packed header")
        rows, channels, block_rows = struct.unpack_from("<QII", packed, 16)
        check(rows == len(members) and channels == 4 and block_rows == 64, "packed dimensions")
        logical_snapshot_bytes += sum(len(components[n]) for n in ("features.i32", "features.sfp", "payload.spp"))
        logical_snapshot_bytes += ((rows + block_rows - 1) // block_rows) * channels * 16
    budgets = {"reuse": sum(capacities.values()), "pressure": capacities[0] + capacities[1]}
    same(suite["cache_budgets"], budgets, "cache budget preparation")

    # Exact historical build bytes are included next to the experiment. Current
    # native files are checked only at relocated paths inside this archive.
    build_data = read(evidence / "MEASURED_BUILD_MANIFEST.json")
    check(digest(build_data) == suite["code_identity"]["build_manifest"]["sha256"], "measured build manifest identity")
    build = json.loads(build_data)
    check(build["status"] == "passed" and build["input_stability"] == "unchanged", "measured build failed/unstable")
    same(build["inputs_before"], build["inputs_after"], "build input stability")
    for name, expected in suite["code_identity"]["project"].items():
        if name in build["inputs_before"]["project"]:
            check(expected == build["inputs_before"]["project"][name], "measured source/build source mismatch")
    check(all(x["status"] == "passed" for x in build["tests"].values()), "recorded build gate failed")
    historic_root = PurePosixPath(suite["output_root"]).parent.parent
    available, absent = {}, []
    expected_binaries = {relative_historical(path, historic_root): expected
                         for path, expected in suite["code_identity"]["binaries"].items()}
    for entry in build["binaries"].values():
        name = relative_historical(entry["path"], historic_root)
        if name in expected_binaries:
            check(expected_binaries[name] == entry["sha256"], "measured executable/build identity mismatch")
        expected_binaries[name] = entry["sha256"]
    for name, expected in expected_binaries.items():
        path = local(project_root, name)
        if path.exists():
            # Instrumented builds are not part of this Release identity catalog.
            check(digest(read(path, 256 << 20)) == expected, "available Release executable differs: " + name)
            available[name] = expected
        else:
            absent.append(name)
    check(not require_binaries or not absent, "required Release executable absent")

    schedule = []
    for pair in range(pairs):
        traces = ("reuse", "pressure") if pair % 2 == 0 else ("pressure", "reuse")
        policies = ("bypass", "populate") if pair % 2 == 0 else ("populate", "bypass")
        for trace in traces:
            for position, policy in enumerate(policies):
                schedule.append((trace, pair, position, policy))
    check(len(run["records"]) == len(schedule), "process inventory cardinality")
    record_paths = {item["path"] for item in run["records"]}
    check(len(record_paths) == len(schedule), "duplicate process path")
    actual_record_paths = {"processes/" + name for name in inventory(evidence / "processes")}
    same(actual_record_paths, record_paths, "exact process inventory")
    records, process_audits = {}, []
    totals = {"processes": 0, "complete_output_checks": 0, "verified_rows": 0, "verified_bytes": 0}
    limits = suite["limits"]
    for item, expected_schedule in zip(run["records"], schedule):
        check(tuple(item[k] for k in ("trace", "pair", "position", "policy")) == expected_schedule, "declared AB/BA/trace schedule")
        data = read(local(evidence, item["path"]))
        check(digest(data) == item["sha256"], "process record hash")
        value = json.loads(data)
        trace, pair, position, policy = expected_schedule
        check(tuple(value[k] for k in ("trace", "pair", "position", "policy")) == expected_schedule and
              value["status"] == "passed" and value["returncode"] == 0, "process state/schedule")
        same(value["code_identity"], suite["code_identity"], "process source identity")
        check(value["cache_budget"] == budgets[trace] and len(value["opens"]) == 12 and
              len(value["requests"]) == len(TRACES[trace]) and len(value["stats_steps"]) == len(TRACES[trace]) + 1,
              "process contract dimensions")
        for index, opened in enumerate(value["opens"]):
            native, project = opened["native"], suite["projects"][index]
            check(opened["project"] == index and native["status"] == "ok" and native["generation"] == project["generation"] and
                  native["manifest_sha256"] == project["manifest_sha256"] and native["rows"] == project["rows"] and
                  native["verified_body_bytes"] == project["plaintext_bytes"], "recorded full admission identity")
        check(value["stats_steps"][0]["after_request"] == -1, "initial stats chronology")
        initial = value["stats_steps"][0]["response"]["stats"]
        check(initial["runs"] == 0 and initial["plan_entries"] == 0 and initial["result_entries"] == 0,
              "initial state not fresh")
        lru = []
        hits = admissions = evictions = multi_victim = compared_rows = compared_bytes = 0
        for index, request in enumerate(value["requests"]):
            project = TRACES[trace][index]
            want = descriptors[project]
            total_bytes = sum(x["bytes"] for x in want)
            check(request["index"] == index and request["project"] == project, "query trace")
            same(request["verification"]["output_rows"], want, "recorded row/path/size/digest descriptors")
            check(request["verification"]["rows"] == len(want) and request["verification"]["bytes"] == total_bytes,
                  "recorded comparison counters")
            native = request["native"]
            check(native["status"] == "ok" and native["generation"] == suite["projects"][project]["generation"] and
                  native["selected_rows"] == len(want) and native["output_bytes"] == total_bytes and
                  not native["cache_build_failed"] and not native["cache_oversized"] and not native["retired_generation"],
                  "native output/retention counters")
            hit = policy == "populate" and project in lru
            admitted = policy == "populate" and not hit
            check(native["cache_hit"] == hit and native["cache_stored"] == admitted and
                  native["decoded_payload_bytes"] == (0 if hit else total_bytes), "cache hit/admission/decoded-byte model")
            if policy == "populate":
                check(native["cache_storage_bytes"] == capacities[project], "complete retained result capacity")
                if hit:
                    hits += 1
                    lru.remove(project)
                else:
                    admissions += 1
                    victims = 0
                    while len(lru) >= limits["result_entries"] or sum(capacities[p] for p in lru) + capacities[project] > budgets[trace]:
                        check(bool(lru), "oversized result in modeled trace")
                        lru.pop(); evictions += 1; victims += 1
                    multi_victim += int(victims > 1)
                lru.insert(0, project)
            step = value["stats_steps"][index + 1]
            check(step["after_request"] == index, "statistics chronology")
            stats = step["response"]["stats"]
            check(stats["active_projects"] == 12 and stats["live_snapshots"] == 12 and
                  stats["snapshot_bytes"] == logical_snapshot_bytes and stats["snapshot_bytes"] <= limits["snapshot_bytes"] and
                  stats["metadata_bytes"] <= limits["metadata_bytes"] and stats["plan_entries"] <= limits["plan_entries"] and
                  stats["result_entries"] == len(lru) and stats["result_bytes"] == sum(capacities[p] for p in lru) and
                  stats["result_bytes"] <= budgets[trace] and stats["cache_hits"] == hits and
                  stats["cache_admissions"] == admissions and stats["cache_evictions"] == evictions and
                  stats["runs"] == index + 1 and not stats["retired_snapshot_bytes"] and not stats["retired_metadata_bytes"],
                  "per-step resource/LRU accounting")
            compared_rows += len(want); compared_bytes += total_bytes
        same(value["final_stats"], value["stats_steps"][-1]["response"]["stats"], "final statistics")
        check(value["verified_rows"] == compared_rows and value["verified_bytes"] == compared_bytes, "process comparison totals")
        for name in TIMES + ("native_admission_ms", "native_query_ms"):
            check(isinstance(value[name], (int, float)) and math.isfinite(value[name]) and value[name] >= 0, "invalid phase time")
        recomputed = {
            "oracle_validation_ms": sum(x["oracle_validation_ms"] for x in value["requests"]),
            "admission_request_ms": sum(x["request_wall_ms"] for x in value["opens"]),
            "query_request_ms": sum(x["request_wall_ms"] for x in value["requests"]),
            "stats_request_ms": sum(x["response"]["client_wall_ms"] for x in value["stats_steps"]),
            "native_admission_ms": sum(x["native"]["wall_ms"] for x in value["opens"]),
            "native_query_ms": sum(x["native"]["wall_ms"] for x in value["requests"]),
            **{"native_" + key: sum(x["native"][key] for x in value["requests"])
               for key in ("plan_ms", "execution_ms", "cache_build_ms")},
        }
        recomputed["service_ms"] = value["raw_session_wall_ms"] - recomputed["oracle_validation_ms"]
        for key, expected in recomputed.items(): same(value[key], expected, "process phase " + key)
        process_audits.append({"path": item["path"], "expected_hits": hits, "expected_admissions": admissions,
                               "expected_evictions": evictions, "multi_victim_admissions": multi_victim,
                               "verified_rows": compared_rows, "verified_bytes": compared_bytes})
        records[trace, pair, policy] = value
        totals["processes"] += 1; totals["complete_output_checks"] += len(TRACES[trace])
        totals["verified_rows"] += compared_rows; totals["verified_bytes"] += compared_bytes
    same(summary["totals"], totals, "summary totals")
    same(summary["process_audits"], process_audits, "summary per-process audits")
    trace_summary = {}
    for trace in TRACES:
        actual = summary["traces"][trace]
        check(actual["request_count"] == len(TRACES[trace]) and actual["cache_budget"] == budgets[trace], "summary trace contract")
        pair_rows = []
        for pair in range(pairs):
            a, b = records[trace, pair, "bypass"], records[trace, pair, "populate"]
            pair_rows.append({"pair": pair, "order": [x["policy"] for x in sorted((a, b), key=lambda x: x["position"])],
                              **{key + "_ratio": a[key] / b[key] for key in RATIOS}})
        same(actual["pairs"], pair_rows, "paired ratios/order")
        ratios = {key: describe([p[key + "_ratio"] for p in pair_rows]) for key in RATIOS}
        same(actual["ratios"], ratios, "descriptive paired statistics")
        for policy in ("bypass", "populate"):
            values = [records[trace, pair, policy] for pair in range(pairs)]
            expected = {
                "median_ms": {key: statistics.median(x[key] for x in values) for key in TIMES},
                "final_stats": [x["final_stats"] for x in values],
                "observed_max_result_bytes": max(step["response"]["stats"]["result_bytes"] for x in values for step in x["stats_steps"]),
                "per_request_cache_hit": [[r["native"]["cache_hit"] for r in x["requests"]] for x in values],
                "per_request_cache_stored": [[r["native"]["cache_stored"] for r in x["requests"]] for x in values],
            }
            same(actual["policies"][policy], expected, "policy summary")
        trace_summary[trace] = {"service_ratio_geometric_mean": ratios["service_ms"]["geometric_mean"],
                                "service_ratio_minimum": ratios["service_ms"]["minimum"],
                                "service_ratio_maximum": ratios["service_ms"]["maximum"],
                                "pairs_favoring_populate": sum(x > 1 for x in ratios["service_ms"]["values"])}
    return {
        "status": "passed", "schema_version": 1, "project_root": str(project_root), "evidence_root": str(evidence),
        "audit_script_sha256": digest(read(Path(__file__))),
        "historical_source_root": suite["source_root"], "historical_external_paths_reread": False,
        "original_byte_scope": "included exact corpus copies match recorded original-source lengths/SHA256; external predecessor tree not reread",
        "output_scope": "revalidated every recorded row/path/length/digest against included original bytes; original experiment performed full byte equality; no historical native output reexecuted",
        "codec_scope": "compressed generation bytes and manifests match frozen hashes; this portable audit does not re-decompress payload frames",
        "build_scope": "exact passed stable-input Release manifest, shared source/build hashes and each available archived Release executable; external compiler/SDK/shared-library paths not reread",
        "verified_counts": {**totals, "input_files": len(actual_inputs), "corpus_files": len(all_members),
                            "frozen_source_files": len(frozen), "generation_manifests": 12,
                            "available_release_binaries": len(available)},
        "logical_snapshot_bytes_recomputed": logical_snapshot_bytes,
        "available_release_binaries": available, "absent_release_binaries": absent,
        "suite_sha256": digest(suite_data), "run_sha256": digest(run_data),
        "summary_sha256": digest(read(evidence / "SUMMARY.json")),
        "build_manifest_sha256": digest(build_data), "traces": trace_summary,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--evidence", type=Path, help="defaults to PROJECT/results/catalog_sessions_v0_11")
    parser.add_argument("--report", type=Path, required=True, help="fresh JSON report path; existing files are never overwritten")
    parser.add_argument("--require-binaries", action="store_true", help="require every archived Release build executable")
    args = parser.parse_args()
    evidence = (args.evidence or args.project_root / "results/catalog_sessions_v0_11").resolve()
    report = args.report.resolve()
    if report.exists():
        parser.error("report already exists; choose a fresh path")
    if any(report.is_relative_to(evidence / name) for name in ("inputs", "measurement_source", "processes")):
        parser.error("report must be outside immutable input/source/process evidence directories")
    started = datetime.now(timezone.utc).isoformat()
    try:
        result = audit(args.project_root, evidence, args.require_binaries)
        code = 0
    except Exception as error:
        result = {"status": "failed", "error": str(error), "evidence_root": str(evidence),
                  "audit_script_sha256": digest(read(Path(__file__)))}
        code = 1
    result["started_utc"] = started
    result["finished_utc"] = datetime.now(timezone.utc).isoformat()
    report.parent.mkdir(parents=True, exist_ok=True)
    with report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps({"status": result["status"], "report": str(report),
                      **({"counts": result["verified_counts"]} if code == 0 else {"error": result["error"]})}))
    return code


if __name__ == "__main__":
    sys.exit(main())
