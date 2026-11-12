#!/usr/bin/env python3
"""Complete-output persistent catalog sessions on a partitioned real-code corpus.

prepare: copy exact predecessor source bytes, capture twelve fixed generations.
run: six AB/BA fresh-process pairs per trace, verify every output byte.
summarize: revalidate evidence identities and recompute descriptive pair ratios.

The source corpus is not a production multi-project or Windows workload. No
system cache flushing, network services, or dependency installation is used.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time
import traceback

from artifact_catalog import CaptureLimits, capture
from catalog_client import CatalogProcess

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = 12
SUFFIXES = {".cpp", ".hpp", ".py", ".suf"}
SOURCE_DIRECTORIES = ("src", "include", "apps", "scripts", "examples")
PROGRAM = "input byte_length 0 0\noutput byte_length > 1024\n"
TRACE_PROJECTS = {
    "reuse": list(range(PROJECTS)) * 4,
    "pressure": [0, 1] * 4 + list(range(2, PROJECTS)) + [0, 1] * 4,
}
LIMITS = {"snapshot_bytes": 64 << 20, "metadata_bytes": 8 << 20,
          "project_slots": PROJECTS, "result_entries": 64, "plan_entries": PROJECTS}


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def save(path, value, *, exclusive=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x" if exclusive else "w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, ensure_ascii=True)
        stream.write("\n")


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def compiled_inputs():
    files = set((ROOT / "include").rglob("*.hpp"))
    files.update((ROOT / "src").glob("*.cpp"))
    files.update((ROOT / "apps/catalog").glob("*.cpp"))
    files.update((ROOT / "apps/catalog").glob("*.hpp"))
    files.add(ROOT / "apps/payload/payload.cpp")
    for name in ("artifact_catalog.py", "payload_store.py", "catalog_client.py",
                 "benchmark_catalog_v0_11.py", "build_payload.py", "codegen_support.py"):
        files.add(ROOT / "scripts" / name)
    return sorted(files)


def current_code(binary, packer):
    result = {"project": {str(p.relative_to(ROOT)): sha256(p) for p in compiled_inputs()},
              "binaries": {str(Path(p).resolve()): sha256(p) for p in (binary, packer)}}
    manifest = Path(binary).resolve().parent / "build_manifest.json"
    if manifest.exists():
        build = load(manifest)
        if build.get("status") != "passed" or build.get("input_stability") != "unchanged":
            raise RuntimeError("native build has not completed with stable inputs")
        result["build_manifest"] = {"path": str(manifest), "sha256": sha256(manifest)}
    else:
        raise RuntimeError("native binary requires its passed build_manifest.json")
    return result


def verify_code(suite):
    actual = current_code(suite["binary"], suite["packer"])
    if actual != suite["code_identity"]:
        raise RuntimeError("producer/client/benchmark/native build identity changed; use new evidence directory")
    return actual


def inputs_identity(directory):
    root = Path(directory) / "inputs"
    return {str(p.relative_to(directory)): {"bytes": p.stat().st_size, "sha256": sha256(p)}
            for p in sorted(root.rglob("*")) if p.is_file()}


def verify_inputs(directory, suite):
    actual = inputs_identity(directory)
    if actual != suite["input_identity"]:
        raise RuntimeError("prepared source/generation input inventory or bytes changed")
    for item in suite["source_files"]:
        path = Path(suite["source_root"]) / item["source_relative"]
        if path.stat().st_size != item["bytes"] or sha256(path) != item["sha256"]:
            raise RuntimeError("original predecessor source bytes changed")
    return actual


def prepare(args):
    source = args.source.resolve()
    binary, packer = args.binary.resolve(), args.packer.resolve()
    directory = args.output.resolve()
    code = current_code(binary, packer)
    directory.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter()
    suite = {
        "schema_version": 1, "release": "0.11.0", "status": "preparing",
        "started_utc": utc_now(), "source_root": str(source),
        "output_root": str(directory), "binary": str(binary), "packer": str(packer),
        "code_identity": code, "program": PROGRAM, "project_count": PROJECTS,
        "source_directories": list(SOURCE_DIRECTORIES), "source_suffixes": sorted(SUFFIXES),
        "partition_rule": "UTF-8 relative-path lexical sort, zero-based index modulo12; original relative paths preserved",
        "corpus_scope": "exact predecessor code files partitioned into12 logical projects; not user production projects",
        "source_files": [], "projects": [], "limits": LIMITS,
        "trace_projects": TRACE_PROJECTS,
    }
    save(directory / "suite.json", suite)
    try:
        files = sorted((p for name in SOURCE_DIRECTORIES for p in (source / name).rglob("*")
                        if p.is_file() and p.suffix in SUFFIXES and "__pycache__" not in p.parts),
                       key=lambda p: p.relative_to(source).as_posix().encode("utf-8"))
        if len(files) < PROJECTS:
            raise RuntimeError("insufficient predecessor source files")
        for index, path in enumerate(files):
            relative = path.relative_to(source).as_posix()
            data = path.read_bytes()
            if not len(data) <= 1 << 20:
                raise RuntimeError("source file exceeds bounded corpus row limit")
            project = index % PROJECTS
            destination = directory / "inputs/projects" / f"project_{project:02d}" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as stream:
                stream.write(data)
            digest = hashlib.sha256(data).hexdigest()
            if sha256(destination) != digest:
                raise RuntimeError("source copy differs")
            suite["source_files"].append({"source_relative": relative, "project": project,
                                          "bytes": len(data), "sha256": digest,
                                          "copied_relative": str(destination.relative_to(directory))})
        for project in range(PROJECTS):
            name = f"project_{project:02d}"
            project_root = directory / "inputs/projects" / name
            catalog_root = directory / "inputs/catalogs" / name
            members = sorted((x for x in suite["source_files"] if x["project"] == project),
                             key=lambda x: x["source_relative"].encode("utf-8"))
            selected = [x for x in members if x["bytes"] > 1024]
            record = {"project": project, "name": name,
                      "source_directory": str(project_root.relative_to(directory)),
                      "catalog_directory": str(catalog_root.relative_to(directory)),
                      "rows": len(members), "plaintext_bytes": sum(x["bytes"] for x in members),
                      "selected_rows": len(selected), "selected_bytes": sum(x["bytes"] for x in selected),
                      "expected_result_capacity": sum(x["bytes"] + 24 for x in selected)}
            begin = time.perf_counter()
            producer = capture(project_root, catalog_root, packer, expected_current="none",
                               limits=CaptureLimits(max_files=128, max_total_decoded_bytes=8 << 20,
                                                    max_generation_bytes=16 << 20, max_catalog_bytes=32 << 20))
            record["capture_wall_ms"] = (time.perf_counter() - begin) * 1000
            record["producer"] = producer
            record["generation"] = producer["generation"]
            manifest = catalog_root / "generations" / producer["generation"] / "manifest.txt"
            record["manifest_sha256"] = sha256(manifest)
            if producer["rows"] != record["rows"] or producer["decoded_payload_bytes"] != record["plaintext_bytes"]:
                raise RuntimeError("capture dimensions differ from independent original inventory")
            suite["projects"].append(record)
        suite["cache_budgets"] = {
            "reuse": sum(x["expected_result_capacity"] for x in suite["projects"]),
            "pressure": sum(x["expected_result_capacity"] for x in suite["projects"][:2]),
        }
        suite["corpus"] = {"files": len(files), "plaintext_bytes": sum(x["bytes"] for x in suite["source_files"]),
                           "selected_files": sum(p["selected_rows"] for p in suite["projects"]),
                           "selected_bytes": sum(p["selected_bytes"] for p in suite["projects"])}
        frozen = directory / "measurement_source"
        for relative, digest in code["project"].items():
            destination = frozen / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
            if sha256(destination) != digest:
                raise RuntimeError("code changed during measurement-source capture")
        suite["input_identity"] = inputs_identity(directory)
        suite["preparation_wall_ms"] = (time.perf_counter() - started) * 1000
        suite["finished_utc"] = utc_now()
        suite["status"] = "prepared"
        if current_code(binary, packer) != code:
            raise RuntimeError("code changed during preparation")
        save(directory / "suite.json", suite)
        print(json.dumps({"status": "prepared", "suite": str(directory / "suite.json"),
                          "corpus": suite["corpus"], "cache_budgets": suite["cache_budgets"]}), flush=True)
    except BaseException as error:
        suite.update(status="failed", error=str(error), traceback=traceback.format_exc())
        save(directory / "suite.json", suite)
        raise


def original_oracle(directory, suite):
    """Read original source independently; never consult producer features/masks."""
    result = {}
    for project in range(PROJECTS):
        members = sorted((x for x in suite["source_files"] if x["project"] == project),
                         key=lambda x: x["source_relative"].encode("utf-8"))
        rows = []
        for row, item in enumerate(members):
            original = (Path(suite["source_root"]) / item["source_relative"]).read_bytes()
            copied = (directory / item["copied_relative"]).read_bytes()
            if original != copied or len(original) != item["bytes"] or hashlib.sha256(original).hexdigest() != item["sha256"]:
                raise RuntimeError("independent original/copy oracle disagreement")
            if len(original) > 1024:
                rows.append({"row": row, "path": item["source_relative"], "data": original})
        result[project] = rows
    return result


def check_output(actual, expected, response):
    if len(actual) != len(expected):
        raise RuntimeError("complete output row count differs")
    details = []
    total = 0
    for got, want in zip(actual, expected):
        if got["row"] != want["row"] or got["path"] != want["path"] or got["data"] != want["data"]:
            raise RuntimeError("complete output row/order/path/byte comparison failed")
        total += len(got["data"])
        details.append({"row": got["row"], "path": got["path"], "bytes": len(got["data"]),
                        "sha256": hashlib.sha256(got["data"]).hexdigest()})
    if response["selected_rows"] != len(expected) or response["output_bytes"] != total:
        raise RuntimeError("native output counters differ from complete original bytes")
    if response["cache_build_failed"] or response["cache_oversized"] or response["retired_generation"]:
        raise RuntimeError("unexpected cache failure/oversize/retired state")
    return {"rows": len(actual), "bytes": total, "comparison": "row_order_path_length_and_every_original_byte",
            "output_rows": details}


def check_stats(stats, budget):
    if stats["active_projects"] != PROJECTS or stats["live_snapshots"] != PROJECTS:
        raise RuntimeError("unexpected live project/snapshot count")
    if stats["snapshot_bytes"] > LIMITS["snapshot_bytes"] or stats["metadata_bytes"] > LIMITS["metadata_bytes"]:
        raise RuntimeError("snapshot/metadata budget invariant failed")
    if stats["result_bytes"] > budget or stats["result_entries"] > LIMITS["result_entries"]:
        raise RuntimeError("global result budget invariant failed")
    if stats["plan_entries"] > LIMITS["plan_entries"] or stats["retired_snapshot_bytes"] or stats["retired_metadata_bytes"]:
        raise RuntimeError("plan/retirement invariant failed")


def run_process(directory, suite, oracle, trace, policy, pair, position, affinity):
    budget = suite["cache_budgets"][trace]
    options = ["--max-snapshot-bytes", LIMITS["snapshot_bytes"],
               "--max-metadata-bytes", LIMITS["metadata_bytes"], "--max-cache-bytes", budget,
               "--max-projects", LIMITS["project_slots"], "--max-cache-entries", LIMITS["result_entries"],
               "--max-plans", LIMITS["plan_entries"]]
    record = {"schema_version": 1, "status": "running", "trace": trace, "policy": policy,
              "pair": pair, "position": position, "started_utc": utc_now(),
              "affinity": affinity, "native_options": list(map(str, options)),
              "cache_budget": budget, "opens": [], "requests": [], "stats_steps": [],
              "code_identity": suite["code_identity"]}
    file = directory / "processes" / f"{trace}_pair{pair:02d}_{position}_{policy}.json"
    session_start = time.perf_counter()
    client = None
    verification_ms = 0.0
    try:
        client = CatalogProcess(suite["binary"], native_options=options)
        record["pid"] = client.process.pid
        ready = client.stats()
        if ready["stats"]["active_projects"] or ready["stats"]["runs"]:
            raise RuntimeError("new native process is not empty")
        record["startup_ready_ms"] = (time.perf_counter() - session_start) * 1000
        record["startup_result"] = ready
        for project in suite["projects"]:
            begin = time.perf_counter()
            response = client.open(project["name"], directory / project["catalog_directory"], expected_revision=0)
            outer = (time.perf_counter() - begin) * 1000
            if (response["generation"] != project["generation"] or response["rows"] != project["rows"] or
                    response["verified_body_bytes"] != project["plaintext_bytes"] or
                    response["manifest_sha256"] != project["manifest_sha256"]):
                raise RuntimeError("admission identity/counters differ")
            record["opens"].append({"project": project["project"], "request_wall_ms": outer, "native": response})
        initial = client.stats()
        check_stats(initial["stats"], budget)
        if initial["stats"]["result_entries"] or initial["stats"]["plan_entries"]:
            raise RuntimeError("session started with a prepared query/result")
        record["stats_steps"].append({"after_request": -1, "response": initial})
        for index, project_index in enumerate(TRACE_PROJECTS[trace]):
            project = suite["projects"][project_index]
            begin = time.perf_counter()
            response, rows = client.run(project["name"], PROGRAM, cache=policy, emit="hex",
                                        max_rows=project["rows"], max_bytes=project["plaintext_bytes"])
            outer = (time.perf_counter() - begin) * 1000
            compare_start = time.perf_counter()
            checked = check_output(rows, oracle[project_index], response)
            verify_time = (time.perf_counter() - compare_start) * 1000
            verification_ms += verify_time
            record["requests"].append({"index": index, "project": project_index,
                                       "request_wall_ms": outer, "oracle_validation_ms": verify_time,
                                       "native": response, "verification": checked})
            observed = client.stats()
            check_stats(observed["stats"], budget)
            record["stats_steps"].append({"after_request": index, "response": observed})
        close_start = time.perf_counter()
        client.close()
        record["close_ms"] = (time.perf_counter() - close_start) * 1000
        record["returncode"] = client.process.returncode
        if record["returncode"] != 0:
            raise RuntimeError("native process did not exit successfully")
        record["raw_session_wall_ms"] = (time.perf_counter() - session_start) * 1000
        record["oracle_validation_ms"] = verification_ms
        record["service_ms"] = record["raw_session_wall_ms"] - verification_ms
        record["admission_request_ms"] = sum(x["request_wall_ms"] for x in record["opens"])
        record["query_request_ms"] = sum(x["request_wall_ms"] for x in record["requests"])
        record["stats_request_ms"] = sum(x["response"]["client_wall_ms"] for x in record["stats_steps"])
        record["native_admission_ms"] = sum(x["native"]["wall_ms"] for x in record["opens"])
        record["native_query_ms"] = sum(x["native"]["wall_ms"] for x in record["requests"])
        record["native_plan_ms"] = sum(x["native"]["plan_ms"] for x in record["requests"])
        record["native_execution_ms"] = sum(x["native"]["execution_ms"] for x in record["requests"])
        record["native_cache_build_ms"] = sum(x["native"]["cache_build_ms"] for x in record["requests"])
        record["verified_rows"] = sum(x["verification"]["rows"] for x in record["requests"])
        record["verified_bytes"] = sum(x["verification"]["bytes"] for x in record["requests"])
        record["final_stats"] = record["stats_steps"][-1]["response"]["stats"]
        if policy == "bypass" and (record["final_stats"]["result_entries"] or record["final_stats"]["cache_hits"]):
            raise RuntimeError("bypass retained/hit results")
        if record["final_stats"]["runs"] != len(TRACE_PROJECTS[trace]):
            raise RuntimeError("completed native run count differs")
        record.update(status="passed", finished_utc=utc_now())
    except BaseException as error:
        record.update(status="failed", error=str(error), traceback=traceback.format_exc(), finished_utc=utc_now())
        if client is not None:
            client.close(force=True)
        save(file, record, exclusive=True)
        raise
    save(file, record, exclusive=True)
    return {"path": str(file.relative_to(directory)), "sha256": sha256(file),
            "trace": trace, "pair": pair, "position": position, "policy": policy}


def execute_sessions(args):
    directory = args.output.resolve()
    suite = load(directory / "suite.json")
    if suite["status"] != "prepared":
        raise RuntimeError("suite is not successfully prepared")
    if not 1 <= args.pairs <= 12:
        raise ValueError("pairs must be in [1,12]")
    run_file = directory / "RUN.json"
    if run_file.exists() or (directory / "processes").exists():
        raise RuntimeError("refusing to overwrite or resume an existing session experiment")
    verify_code(suite)
    verify_inputs(directory, suite)
    oracle = original_oracle(directory, suite)
    original_affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    affinity = [original_affinity[0]] if original_affinity else None
    if affinity:
        os.sched_setaffinity(0, affinity)
    run = {"schema_version": 1, "status": "running", "started_utc": utc_now(), "pairs_per_trace": args.pairs,
           "suite_sha256": sha256(directory / "suite.json"), "records": [], "original_affinity": original_affinity,
           "measurement_affinity": affinity, "platform": platform.platform(), "python": sys.version,
           "processor": platform.processor(), "code_before": verify_code(suite), "inputs_before": verify_inputs(directory, suite),
           "schedule": "pairs alternate bypass/populate and populate/bypass; trace order alternates by pair",
           "timing_contract": {
               "primary": "service_ms = process spawn-through-close wall minus explicitly timed full-output oracle comparisons",
               "included": "process readiness, all12Full admissions, planning, native execution, two-pass Populate construction, full hex output, client JSON/hex decoding/materialization, STATS protocol after every query, close and control bookkeeping",
               "excluded": "one-time corpus capture/packing (reported in suite), frozen identity checks before/after pairs, original-oracle loading, explicitly timed full-output equality/digest verification",
               "raw": "raw_session_wall_ms includes oracle comparisons",
               "cache_state": "fresh native processes; filesystem page caches are uncontrolled and not flushed",
               "affinity": "Python harness and inherited native child share one permitted logical CPU; not exclusive CPU ownership",
               "uncertainty": "six paired process observations per trace are descriptive; ranges are not confidence intervals",
           }}
    save(run_file, run, exclusive=True)
    try:
        for pair in range(args.pairs):
            traces = ("reuse", "pressure") if pair % 2 == 0 else ("pressure", "reuse")
            policies = ("bypass", "populate") if pair % 2 == 0 else ("populate", "bypass")
            for trace in traces:
                verify_code(suite)
                verify_inputs(directory, suite)
                for position, policy in enumerate(policies):
                    print(f"{trace} pair{pair} position{position} {policy}", flush=True)
                    result = run_process(directory, suite, oracle, trace, policy, pair, position, affinity)
                    run["records"].append(result)
                    save(run_file, run)
                verify_code(suite)
                verify_inputs(directory, suite)
        run["code_after"] = verify_code(suite)
        run["inputs_after"] = verify_inputs(directory, suite)
        run.update(status="passed", finished_utc=utc_now())
    except BaseException as error:
        run.update(status="failed", error=str(error), traceback=traceback.format_exc(), finished_utc=utc_now())
        raise
    finally:
        save(run_file, run)
        if original_affinity is not None:
            os.sched_setaffinity(0, original_affinity)
    summarize(directory)


def descriptive(values):
    if not values or not all(math.isfinite(x) and x > 0 for x in values):
        raise RuntimeError("ratio observation must be finite and positive")
    return {"values": values, "median": statistics.median(values), "minimum": min(values), "maximum": max(values),
            "geometric_mean": math.exp(sum(math.log(x) for x in values) / len(values))}


def audit_process(value, suite, expected_rows):
    """Independent byte-oracle descriptors and a small exact byte-LRU model."""
    trace, policy = value["trace"], value["policy"]
    if trace not in TRACE_PROJECTS or policy not in {"bypass", "populate"}:
        raise RuntimeError("unknown recorded trace/policy")
    if len(value["opens"]) != PROJECTS or len(value["stats_steps"]) != len(TRACE_PROJECTS[trace]) + 1:
        raise RuntimeError("admission/stats inventory incomplete")
    for index, opened in enumerate(value["opens"]):
        expected = suite["projects"][index]
        response = opened["native"]
        if (opened["project"] != index or response["generation"] != expected["generation"] or
                response["manifest_sha256"] != expected["manifest_sha256"] or response["rows"] != expected["rows"] or
                response["verified_body_bytes"] != expected["plaintext_bytes"] or response["status"] != "ok"):
            raise RuntimeError("recorded admission disagrees with frozen input")
    lru = []  # most recent first, project-specific sole query/range
    hits = admissions = evictions = checked_rows = checked_bytes = multi_victim_events = 0
    capacities = {p["project"]: p["expected_result_capacity"] for p in suite["projects"]}
    budget = suite["cache_budgets"][trace]
    for index, request in enumerate(value["requests"]):
        project = TRACE_PROJECTS[trace][index]
        if request["index"] != index or request["project"] != project:
            raise RuntimeError("recorded request index/project differs")
        wanted = expected_rows[project]
        expected_bytes = sum(x["bytes"] for x in wanted)
        verification, response = request["verification"], request["native"]
        if (verification["output_rows"] != wanted or verification["rows"] != len(wanted) or
                verification["bytes"] != expected_bytes or response["selected_rows"] != len(wanted) or
                response["output_bytes"] != expected_bytes or response["status"] != "ok" or
                response["generation"] != suite["projects"][project]["generation"]):
            raise RuntimeError("recorded complete-output evidence differs from independently loaded original bytes")
        checked_rows += len(wanted)
        checked_bytes += expected_bytes
        hit = policy == "populate" and project in lru
        admitted = policy == "populate" and not hit
        if (response["cache_hit"] != hit or response["cache_stored"] != admitted or
                response["cache_oversized"] or response["cache_build_failed"] or response["retired_generation"] or
                response["decoded_payload_bytes"] != (0 if hit else expected_bytes)):
            raise RuntimeError("recorded cache behavior differs from independent trace model")
        if policy == "populate":
            if response["cache_storage_bytes"] != capacities[project]:
                raise RuntimeError("actual retained result capacity differs from prepared byte model")
            if hit:
                hits += 1
                lru.remove(project)
            else:
                admissions += 1
                victims = 0
                while (len(lru) >= LIMITS["result_entries"] or
                       sum(capacities[p] for p in lru) + capacities[project] > budget):
                    if not lru:
                        raise RuntimeError("prepared result does not fit cache budget")
                    lru.pop()
                    evictions += 1
                    victims += 1
                multi_victim_events += int(victims > 1)
            lru.insert(0, project)
        stats = value["stats_steps"][index + 1]
        if stats["after_request"] != index:
            raise RuntimeError("stats step chronology differs")
        stats = stats["response"]["stats"]
        check_stats(stats, budget)
        if (stats["result_bytes"] != sum(capacities[p] for p in lru) or stats["result_entries"] != len(lru) or
                stats["cache_hits"] != hits or stats["cache_admissions"] != admissions or
                stats["cache_evictions"] != evictions or stats["runs"] != index + 1):
            raise RuntimeError("retained cache stats differ from independent LRU/byte model")
    if value["verified_rows"] != checked_rows or value["verified_bytes"] != checked_bytes:
        raise RuntimeError("complete comparison totals differ")
    if value["final_stats"] != value["stats_steps"][-1]["response"]["stats"]:
        raise RuntimeError("final stats differ from final observed step")
    return {"expected_hits": hits, "expected_admissions": admissions, "expected_evictions": evictions,
            "multi_victim_admissions": multi_victim_events, "verified_rows": checked_rows, "verified_bytes": checked_bytes}


def summarize(directory):
    directory = Path(directory)
    suite, run = load(directory / "suite.json"), load(directory / "RUN.json")
    if run["status"] != "passed" or sha256(directory / "suite.json") != run["suite_sha256"]:
        raise RuntimeError("run/suite is incomplete or changed")
    if run["code_before"] != run["code_after"] or run["inputs_before"] != run["inputs_after"]:
        raise RuntimeError("experiment source/input identities changed")
    verify_inputs(directory, suite)
    for relative, digest in suite["code_identity"]["project"].items():
        if sha256(directory / "measurement_source" / relative) != digest:
            raise RuntimeError("frozen measured source differs")
    original = original_oracle(directory, suite)
    expected_rows = {project: [{"row": row["row"], "path": row["path"], "bytes": len(row["data"]),
                               "sha256": hashlib.sha256(row["data"]).hexdigest()} for row in rows]
                     for project, rows in original.items()}
    if len(run["records"]) != 4 * run["pairs_per_trace"]:
        raise RuntimeError("process inventory incomplete")
    records = {}
    audits = []
    totals = {"processes": 0, "complete_output_checks": 0, "verified_rows": 0, "verified_bytes": 0}
    for item in run["records"]:
        file = directory / item["path"]
        if sha256(file) != item["sha256"]:
            raise RuntimeError("retained process evidence changed")
        value = load(file)
        key = value["trace"], value["pair"], value["policy"]
        if key in records or value["status"] != "passed":
            raise RuntimeError("duplicate or unsuccessful process record")
        if value["code_identity"] != suite["code_identity"]:
            raise RuntimeError("process code identities differ")
        if [x["project"] for x in value["requests"]] != TRACE_PROJECTS[value["trace"]]:
            raise RuntimeError("retained request trace differs")
        if abs(value["service_ms"] - (value["raw_session_wall_ms"] - value["oracle_validation_ms"])) > 1e-8:
            raise RuntimeError("service subtraction differs")
        audits.append({"path": item["path"], **audit_process(value, suite, expected_rows)})
        check_stats(value["final_stats"], suite["cache_budgets"][value["trace"]])
        records[key] = value
        totals["processes"] += 1
        totals["complete_output_checks"] += len(value["requests"])
        totals["verified_rows"] += value["verified_rows"]
        totals["verified_bytes"] += value["verified_bytes"]
    result = {"schema_version": 1, "status": "passed", "suite_sha256": run["suite_sha256"],
              "run_sha256": sha256(directory / "RUN.json"), "corpus": suite["corpus"], "totals": totals,
              "ratio_definition": "same-pair persistent_bypass / persistent_populate; above1favorsPopulate; descriptive observed range, not CI",
              "process_audits": audits, "traces": {}}
    for trace in TRACE_PROJECTS:
        pair_rows = []
        for pair in range(run["pairs_per_trace"]):
            a, b = records[trace, pair, "bypass"], records[trace, pair, "populate"]
            if a["position"] == b["position"] or sorted((a["position"], b["position"])) != [0, 1]:
                raise RuntimeError("pair order invalid")
            pair_rows.append({"pair": pair, "order": [x["policy"] for x in sorted((a, b), key=lambda x: x["position"])],
                              **{name + "_ratio": a[name] / b[name] for name in
                                 ("service_ms", "query_request_ms", "native_query_ms", "raw_session_wall_ms")}})
        trace_result = {"request_count": len(TRACE_PROJECTS[trace]), "cache_budget": suite["cache_budgets"][trace],
                        "pairs": pair_rows,
                        "ratios": {name: descriptive([p[name + "_ratio"] for p in pair_rows]) for name in
                                   ("service_ms", "query_request_ms", "native_query_ms", "raw_session_wall_ms")},
                        "policies": {}}
        for policy in ("bypass", "populate"):
            values = [records[trace, pair, policy] for pair in range(run["pairs_per_trace"])]
            trace_result["policies"][policy] = {
                "median_ms": {key: statistics.median(x[key] for x in values) for key in
                              ("service_ms", "raw_session_wall_ms", "oracle_validation_ms", "startup_ready_ms",
                               "admission_request_ms", "query_request_ms", "stats_request_ms", "close_ms",
                               "native_plan_ms", "native_execution_ms", "native_cache_build_ms")},
                "final_stats": [x["final_stats"] for x in values],
                "observed_max_result_bytes": max(s["response"]["stats"]["result_bytes"] for x in values for s in x["stats_steps"]),
                "per_request_cache_hit": [[r["native"]["cache_hit"] for r in x["requests"]] for x in values],
                "per_request_cache_stored": [[r["native"]["cache_stored"] for r in x["requests"]] for x in values],
            }
        result["traces"][trace] = trace_result
    save(directory / "SUMMARY.json", result)
    lines = ["# Suffice0.11 complete-session observations", "",
             "Ratios are paired bypass/populate; above1favorsPopulate. Ranges are six observed process-pair minima/maxima, not confidence intervals.", "",
             "| Trace | Service ratio geometric mean | Observed range | Query-request ratio | Bypass service median ms | Populate service median ms |",
             "|---|---:|---:|---:|---:|---:|"]
    for trace, item in result["traces"].items():
        ratio = item["ratios"]["service_ms"]
        lines.append(f"| {trace} | {ratio['geometric_mean']:.6f} | {ratio['minimum']:.6f}–{ratio['maximum']:.6f} | "
                     f"{item['ratios']['query_request_ms']['geometric_mean']:.6f} | "
                     f"{item['policies']['bypass']['median_ms']['service_ms']:.3f} | "
                     f"{item['policies']['populate']['median_ms']['service_ms']:.3f} |")
    lines += ["", "Complete service includes all12Full admissions, startup/close, every output byte through the Python client, cache construction, and STATS instrumentation. It excludes explicitly timed oracle equality/digest checks; raw wall retains them. Corpus capture is one-time preparation and reported separately. No production/Windows/general speedup inference follows.", "",
              "```json", json.dumps(totals, indent=2), "```", ""]
    (directory / "TABLES.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({"status": "passed", "summary": str(directory / "SUMMARY.json"), "totals": totals,
                      "service_ratios": {name: x["ratios"]["service_ms"] for name, x in result["traces"].items()}}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("prepare")
    create.add_argument("--source", type=Path, required=True)
    create.add_argument("--binary", type=Path, required=True)
    create.add_argument("--packer", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    run = commands.add_parser("run")
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--pairs", type=int, default=6)
    summary = commands.add_parser("summarize")
    summary.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "prepare": prepare(args)
    elif args.command == "run": execute_sessions(args)
    else: summarize(args.output.resolve())


if __name__ == "__main__":
    main()
