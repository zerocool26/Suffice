#!/usr/bin/env python3
"""Paired complete-output service experiment for JSONL and bounded binary transport.

Run (the default command) requires a new output directory and a passed,
stable-input native Release build. Audit/summarize use frozen local evidence
only: they do not invoke a native executable, reread historical external paths,
or reproduce historical timings. See docs/transport_experiment_v0_13.md.
"""
from __future__ import annotations

import argparse
from collections import OrderedDict
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import statistics
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = 12
THRESHOLDS = (1024, 8192, 65536, 1 << 30)
SCENARIOS = ("reuse", "pressure", "mixed_queries", "computed", "empty")
LIMITS = {"snapshot_bytes": 64 << 20, "metadata_bytes": 8 << 20,
          "project_slots": 12, "result_entries": 64, "plan_entries": 64,
          "worker_entries": 32}
VARIANTS = (
    {"name": "jsonl", "transport": "jsonl", "population": "capture", "worker_bytes": 16 << 20},
    {"name": "binary", "transport": "binary", "population": "capture", "worker_bytes": 16 << 20},
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def object_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     ensure_ascii=True).encode("utf-8")).hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def save(path, value, *, exclusive=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    if exclusive:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
    else:
        temporary = path.with_name(path.name + ".writing")
        with temporary.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
        temporary.replace(path)


def relative_file(base, relative):
    require(isinstance(relative, str) and relative and "\\" not in relative and ":" not in relative
            and not any(ord(c) < 32 or ord(c) == 127 for c in relative),
            "invalid evidence-relative path")
    value = PurePosixPath(relative)
    require(not value.is_absolute() and not any(p in (".", "..") for p in value.parts)
            and value.as_posix() == relative, "noncanonical evidence-relative path")
    path = Path(base)
    require(path.is_dir() and not path.is_symlink(), "evidence base is not an ordinary directory")
    resolved_base = path.resolve()
    for part in value.parts:
        path = path / part
        require(not path.is_symlink(), "symbolic links are not frozen evidence")
    require(path.is_file(), "missing regular evidence file: " + relative)
    require(path.resolve().is_relative_to(resolved_base), "evidence path escapes its base")
    return path


def input_inventory(directory):
    root = Path(directory) / "inputs"
    require(root.is_dir() and not root.is_symlink(), "missing ordinary input directory")
    result = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), "input tree contains a symbolic link")
        if path.is_dir():
            continue
        require(path.is_file(), "input tree contains a nonregular object")
        result[path.relative_to(directory).as_posix()] = {
            "bytes": path.stat().st_size, "sha256": sha256(path)}
    return result


def current_code(binary):
    """Check every project input recorded by the completed build, plus Python."""
    binary = Path(binary).resolve()
    manifest_path = binary.parent / "build_manifest.json"
    build = load(manifest_path)
    require(build.get("status") == "passed" and build.get("input_stability") == "unchanged",
            "native build is not passed with unchanged inputs")
    require(build.get("tests_requested") is True and build.get("tests")
            and all(t.get("status") == "passed" for t in build["tests"].values()),
            "benchmark requires the recorded build verification gates to pass")
    require(build.get("sanitize") is False, "timed experiment requires a nonsanitized Release build")
    require(Path(build["root"]).resolve() == ROOT, "build root differs from this benchmark's project")
    require(build["inputs_before"] == build["inputs_after"], "build input snapshots differ")
    project = build["inputs_after"]["project"]
    require(isinstance(project, dict) and project, "build has no compiled-project input inventory")
    for relative, digest in project.items():
        require(sha256(relative_file(ROOT, relative)) == digest,
                "compiled project input changed since build: " + relative)
    python_sources = {}
    for relative in ("scripts/catalog_client.py", "scripts/benchmark_transport_v0_13.py"):
        python_sources[relative] = sha256(relative_file(ROOT, relative))
    actual_binary = sha256(binary)
    built = build["binaries"].get("suffice_catalog", {})
    require(built.get("sha256") == actual_binary
            and Path(built.get("path", "")).resolve() == binary,
            "catalog binary is not the binary identified by its build manifest")
    return {"project": dict(sorted(project.items())), "python": python_sources,
            "methods": {p: sha256(relative_file(ROOT, p)) for p in
                        ("docs/binary_protocol_v0_13.md", "docs/transport_experiment_v0_13.md")},
            "binary": {"path": str(binary), "sha256": actual_binary},
            "build_manifest": {"path": str(manifest_path), "sha256": sha256(manifest_path)},
            "external_scope": "compiler, SDK, codec and shared-library details retained in build manifest; not reread as project source"}


def trace(scenario):
    if scenario in ("reuse", "computed", "empty"):
        threshold = (1 << 30) if scenario == "empty" else 1024
        return [(p, threshold) for p in list(range(PROJECTS)) * 4]
    if scenario == "pressure":
        return [(p, 1024) for p in [0, 1] * 4 + list(range(2, PROJECTS)) + [0, 1] * 4]
    if scenario == "mixed_queries":
        return [(p, threshold) for threshold in THRESHOLDS[:3] for p in range(PROJECTS)] * 2
    raise RuntimeError("unknown scenario")

def program(threshold):
    return f"input byte_length 0 0\noutput byte_length > {threshold}\n"


def schedule(repeats):
    items = []
    for repetition in range(repeats):
        rotation = repetition % len(SCENARIOS)
        rotated = SCENARIOS[rotation:] + SCENARIOS[:rotation]
        order = (0, 1) if repetition % 2 == 0 else (1, 0)
        for scenario_position, scenario in enumerate(rotated):
            for position, index in enumerate(order):
                items.append({"ordinal": len(items), "repetition": repetition,
                              "scenario": scenario, "scenario_position": scenario_position,
                              "position": position, "variant": dict(VARIANTS[index]),
                              "policy": "bypass" if scenario in ("computed", "empty") else "populate"})
    return items

def original_oracle(directory, source_files):
    """Select by original body length; never consult captured derived features."""
    require(len(source_files) == 103, "frozen corpus must contain exactly 103 original files")
    ordered = sorted(source_files, key=lambda x: x["source_relative"].encode("utf-8"))
    require(ordered == source_files, "original source inventory is not UTF-8 path sorted")
    require(len({x["source_relative"] for x in ordered}) == len(ordered), "duplicate original source path")
    by_project = {p: [] for p in range(PROJECTS)}
    for index, item in enumerate(ordered):
        project = index % PROJECTS
        require(item["project"] == project, "original round-robin partition differs")
        wanted_path = f"inputs/projects/project_{project:02d}/" + item["source_relative"]
        require(item["copied_relative"] == wanted_path, "copied original path differs")
        body = relative_file(directory, wanted_path).read_bytes()
        require(len(body) == item["bytes"] and hashlib.sha256(body).hexdigest() == item["sha256"],
                "copied original body differs from preserved source record")
        by_project[project].append({"path": item["source_relative"], "data": body})
    result = {}
    for project, members in by_project.items():
        members.sort(key=lambda x: x["path"].encode("utf-8"))
        for threshold in THRESHOLDS:
            result[project, threshold] = [
                {"row": row, "path": item["path"], "data": item["data"]}
                for row, item in enumerate(members) if len(item["data"]) > threshold]
    return result


def describe_rows(rows):
    return [{"row": row["row"], "path": row["path"], "bytes": len(row["data"]),
             "sha256": hashlib.sha256(row["data"]).hexdigest()} for row in rows]


def capacities(oracle):
    return {key: sum(24 + len(row["data"]) for row in rows) for key, rows in oracle.items()}


def budgets(values):
    return {"reuse": sum(values[p, 1024] for p in range(PROJECTS)),
            "pressure": values[0, 1024] + values[1, 1024],
            "mixed_queries": sum(values.values()), "computed": 0, "empty": 0}

def resource_check(stats, variant, budget, projects=PROJECTS):
    require(stats["active_projects"] == stats["live_snapshots"] == stats["live_generations"] == projects,
            "unexpected live project/generation/snapshot count")
    require(stats["snapshot_bytes"] <= LIMITS["snapshot_bytes"]
            and stats["metadata_bytes"] <= LIMITS["metadata_bytes"], "snapshot/metadata budget exceeded")
    require(stats["result_bytes"] <= budget and stats["result_entries"] <= LIMITS["result_entries"],
            "result retention bound exceeded")
    require(stats["plan_worker_bytes"] <= variant["worker_bytes"]
            and stats["worker_entries"] <= LIMITS["worker_entries"], "worker retention bound exceeded")
    require(stats["plan_entries"] <= LIMITS["plan_entries"]
            and stats["worker_entries"] <= stats["plan_entries"], "plan/worker count inconsistent")
    require(stats["retired_snapshot_bytes"] == stats["retired_metadata_bytes"] == 0,
            "unexpected retired generation charge")


class ResultModel:
    """Exact byte/count LRU model independent of native replacement events."""
    def __init__(self, values, budget, policy):
        self.values, self.budget, self.policy = values, budget, policy
        self.lru = OrderedDict()  # oldest first
        self.hits = self.misses = self.admissions = self.evictions = 0
        self.multivictim = 0
        self.rows = self.bytes = self.runs = 0

    def observe(self, key, expected, response, stats, variant):
        rows, body_bytes = len(expected), sum(x["bytes"] for x in expected)
        hit = self.policy != "bypass" and key in self.lru
        admit = self.policy == "populate" and not hit
        require(response["cache_hit"] == hit and response["cache_stored"] == admit,
                "native hit/admission differs from independent LRU model")
        require(not response["cache_oversized"] and not response["cache_build_failed"]
                and not response["retired_generation"], "unexpected failed/oversized/retired request")
        require(response["decoded_payload_bytes"] == (0 if hit else body_bytes),
                "normal execution decoded-byte counter differs")
        require(response["population_strategy"] == variant["population"], "population strategy differs")
        require(response["cache_storage_bytes"] == (0 if self.policy == "bypass" else self.values[key]),
                "actual result capacity differs from 24-byte rows plus plaintext model")
        victims = []
        if self.policy != "bypass":
            if hit:
                self.hits += 1
                self.lru.move_to_end(key)
            else:
                self.misses += 1
                self.admissions += 1
                require(self.values[key] <= self.budget, "candidate does not fit expected result budget")
                while (len(self.lru) >= LIMITS["result_entries"]
                       or sum(self.lru.values()) + self.values[key] > self.budget):
                    require(bool(self.lru), "LRU victim model exhausted unexpectedly")
                    victim, _ = self.lru.popitem(last=False)
                    victims.append(list(victim))
                    self.evictions += 1
                self.lru[key] = self.values[key]
                self.multivictim += int(len(victims) > 1)
        self.runs += 1
        self.rows += rows
        self.bytes += body_bytes
        resource_check(stats, variant, self.budget)
        require(stats["result_bytes"] == sum(self.lru.values())
                and stats["result_entries"] == len(self.lru), "result occupancy differs from LRU model")
        require(stats["cache_hits"] == self.hits and stats["cache_misses"] == self.misses
                and stats["cache_admissions"] == self.admissions and stats["cache_evictions"] == self.evictions
                and stats["runs"] == self.runs, "result counters differ from independent model")
        require(stats["cache_invalidations"] == stats["plan_invalidations"] == stats["worker_invalidations"] == 0,
                "unexpected invalidation in an immutable trace")
        if hit:
            require(not response["worker_reused"] and not response["worker_retained"]
                    and response["worker_scratch_bytes"] == 0, "result hit unexpectedly used a worker")
        if response["worker_oversized"]:
            require(not response["worker_retained"], "oversized worker was retained")
        return {"hit": hit, "admitted": admit, "victims": victims,
                "result_bytes": sum(self.lru.values()), "result_entries": len(self.lru)}


def check_output(actual, expected, response):
    require(len(actual) == len(expected), "full-output row count differs")
    for got, wanted in zip(actual, expected):
        require(got["row"] == wanted["row"] and got["path"] == wanted["path"]
                and got["data"] == wanted["data"], "row/order/path/full-original-byte comparison failed")
    descriptors = describe_rows(actual)
    total = sum(x["bytes"] for x in descriptors)
    require(response["status"] == "ok" and response["selected_rows"] == len(actual)
            and response["output_bytes"] == total, "native full-output counters differ")
    return {"rows": len(actual), "bytes": total, "output_rows": descriptors,
            "comparison": "row_order_path_length_and_every_original_byte"}


def open_check(response, project):
    require(response["status"] == "ok" and response["generation"] == project["generation"]
            and response["rows"] == project["rows"]
            and response["verified_body_bytes"] == project["plaintext_bytes"]
            and response["manifest_sha256"] == project["manifest_sha256"],
            "admission identity or logical counters differ from frozen inputs")


def native_options(variant, budget):
    return list(map(str, ["--population", variant["population"],
        "--max-worker-bytes", variant["worker_bytes"], "--max-worker-entries", LIMITS["worker_entries"],
        "--max-plans", LIMITS["plan_entries"], "--max-cache-entries", LIMITS["result_entries"],
        "--max-cache-bytes", budget, "--max-snapshot-bytes", LIMITS["snapshot_bytes"],
        "--max-metadata-bytes", LIMITS["metadata_bytes"], "--max-projects", LIMITS["project_slots"]]))


def wire_check(response, rows, transport):
    """Independent expected row encoding size; control lengths are retained counters."""
    for field in ("request_wire_bytes", "response_wire_bytes", "response_row_wire_bytes", "response_control_wire_bytes"):
        require(type(response.get(field)) is int and response[field] >= 0, "invalid wire byte counter")
    require(response["response_wire_bytes"] == response["response_row_wire_bytes"] + response["response_control_wire_bytes"],
            "response wire partition differs")
    require(response["response_control_wire_bytes"] > (16 if transport == "binary" else 1),
            "terminal response wire length is impossible")
    expected = 0
    for row in rows:
        path = row["path"].encode("utf-8")
        if transport == "binary":
            expected += 16 + 32 + len(path) + row["bytes"]
        else:
            empty_body = {"type": "row", "seq": response["seq"], "row": row["row"],
                          "path_hex": path.hex(), "data_hex": ""}
            expected += len(json.dumps(empty_body, separators=(",", ":"), ensure_ascii=True).encode("ascii")) + 1 + 2 * row["bytes"]
    require(response["response_row_wire_bytes"] == expected, "measured row wire bytes differ from independent encoding formula")


def reconcile_wire(record):
    responses = [record["startup_result"]]
    for entry in record["opens"] + record["requests"]:
        responses.extend((entry["native"], entry["stats_response"]))
    responses.append(record["close_result"])
    require([x["seq"] for x in responses] == list(range(1, len(responses) + 1)), "response sequence inventory differs")
    received = sum(x["response_wire_bytes"] for x in responses)
    sent = sum(x["request_wire_bytes"] for x in responses)
    require(received == record["wire_bytes_received"] and sent == record["wire_bytes_sent"],
            "cumulative pipe counters differ from every request including QUIT")
    return {"responses": len(responses), "response_bytes": received, "request_bytes": sent,
            "row_bytes": sum(x["response_row_wire_bytes"] for x in responses),
            "control_bytes": sum(x["response_control_wire_bytes"] for x in responses),
            "query_response_bytes": sum(x["native"]["response_wire_bytes"] for x in record["requests"])}


def prepare(args):
    evidence = args.evidence_source.resolve()
    # A completed bundle carries the unchanged original provenance alongside
    # inputs/, so a fresh experiment never needs the old absolute source tree.
    if (evidence / "EVIDENCE_SOURCE_SUITE.json").is_file():
        old_suite_path = relative_file(evidence, "EVIDENCE_SOURCE_SUITE.json")
        old_run_path = relative_file(evidence, "EVIDENCE_SOURCE_RUN.json")
    else:
        old_suite_path = relative_file(evidence, "suite.json")
        old_run_path = relative_file(evidence, "RUN.json")
    old, old_run = load(old_suite_path), load(old_run_path)
    require(old.get("status") == "prepared" and old_run.get("status") == "passed",
            "source experiment is not a completed verified experiment")
    require(old_run["suite_sha256"] == sha256(old_suite_path), "source experiment suite changed")
    require(old["project_count"] == PROJECTS and old["corpus"]["files"] == 103,
            "source experiment does not match the declared frozen corpus")
    old_inventory = input_inventory(evidence)
    require(old_inventory == old["input_identity"] and len(old_inventory) == 187,
            "source experiment inputs differ from frozen identities")
    original_oracle(evidence, old["source_files"])
    code = current_code(args.binary)
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    started = time.perf_counter_ns()
    suite = {"schema_version": 1, "experiment": "transport_v0_13", "status": "preparing",
             "release": "0.13.0", "started_utc": utc_now(), "binary": str(args.binary.resolve()),
             "source_evidence": {"historical_root": str(evidence), "suite_sha256": sha256(old_suite_path),
                                 "run_sha256": sha256(old_run_path), "inputs": old_inventory},
             "code_identity": code, "code_identity_sha256": object_hash(code),
             "limits": LIMITS, "result_row_descriptor_bytes": 24,
             "source_files": old["source_files"], "projects": old["projects"],
             "corpus": old["corpus"], "variants": list(VARIANTS),
             "thresholds": list(THRESHOLDS), "repeats": args.repeats,
             "traces": {name: [list(key) for key in trace(name)] for name in SCENARIOS},
             "schedule": schedule(args.repeats),
             "source_scope": "Exact copied predecessor bodies and immutable stores; no recapture, re-encoding or external predecessor-tree reads"}
    save(directory / "suite.json", suite, exclusive=True)
    try:
        shutil.copytree(evidence / "inputs", directory / "inputs")
        shutil.copyfile(old_suite_path, directory / "EVIDENCE_SOURCE_SUITE.json")
        shutil.copyfile(old_run_path, directory / "EVIDENCE_SOURCE_RUN.json")
        shutil.copyfile(code["build_manifest"]["path"], directory / "MEASURED_BUILD_MANIFEST.json")
        frozen = dict(code["project"])
        frozen.update(code["python"])
        frozen.update(code["methods"])
        for relative, digest in frozen.items():
            destination = directory / "measurement_source" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(relative_file(ROOT, relative), destination)
            require(sha256(destination) == digest, "project source changed while freezing")
        suite["frozen_source_hashes"] = dict(sorted(frozen.items()))
        suite["input_identity"] = input_inventory(directory)
        require(suite["input_identity"] == old_inventory, "copied inputs differ")
        oracle = original_oracle(directory, suite["source_files"])
        values = capacities(oracle)
        suite["cache_budgets"] = budgets(values)
        suite["query_results"] = [{"project": key[0], "threshold": key[1],
                                    "capacity": values[key], "output_rows": describe_rows(rows)}
                                   for key, rows in sorted(oracle.items())]
        require(current_code(args.binary) == code and input_inventory(evidence) == old_inventory
                and sha256(old_suite_path) == suite["source_evidence"]["suite_sha256"]
                and sha256(old_run_path) == suite["source_evidence"]["run_sha256"],
                "source or build identities changed during copying")
        suite.update(status="prepared", finished_utc=utc_now(),
                     preparation_wall_ms=(time.perf_counter_ns() - started) / 1e6)
        save(directory / "suite.json", suite)
        return directory, suite, oracle, values
    except BaseException as error:
        suite.update(status="failed", error=str(error), traceback=traceback.format_exc())
        save(directory / "suite.json", suite)
        raise


def run_process(directory, suite, oracle, values, item, affinity):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from catalog_client import CatalogProcess

    scenario, variant = item["scenario"], item["variant"]
    budget = suite["cache_budgets"][scenario]
    model = ResultModel(values, budget, item["policy"])
    record = {"schema_version": 1, "status": "running", "schedule": item,
              "started_utc": utc_now(), "code_identity_sha256": suite["code_identity_sha256"],
              "native_options": native_options(variant, budget), "affinity": affinity,
              "cache_budget": budget, "opens": [], "requests": []}
    file = directory / "processes" / f"{item['ordinal']:03d}_{scenario}_r{item['repetition']:02d}_{variant['name']}.json"
    audit_ns = 0

    def checked(function, *arguments):
        nonlocal audit_ns
        begin = time.perf_counter_ns()
        result = function(*arguments)
        elapsed = time.perf_counter_ns() - begin
        audit_ns += elapsed
        return result, elapsed / 1e6

    client = None
    started = time.perf_counter_ns()
    try:
        client = CatalogProcess(suite["binary"], native_options=record["native_options"],
                                transport=variant["transport"])
        record["pid"] = client.process.pid
        record["native_argv"] = client.process.args
        record["child_affinity"] = sorted(os.sched_getaffinity(client.process.pid))
        require(record["child_affinity"] == affinity, "child did not inherit the pinned CPU")
        ready = client.stats()
        record["startup_ready_ms"] = (time.perf_counter_ns() - started) / 1e6
        record["startup_result"] = ready
        _, startup_resource_audit = checked(resource_check, ready["stats"], variant, budget, 0)
        _, startup_wire_audit = checked(wire_check, ready, (), variant["transport"])
        record["startup_audit_ms"] = startup_resource_audit + startup_wire_audit
        require(ready["stats"]["runs"] == 0, "new native process has completed requests")
        for number, project in enumerate(suite["projects"]):
            begin = time.perf_counter_ns()
            response = client.open(project["name"], directory / project["catalog_directory"], expected_revision=0)
            wall_ms = (time.perf_counter_ns() - begin) / 1e6
            stats_response = client.stats()
            _, admission_audit = checked(open_check, response, project)
            _, open_wire_audit = checked(wire_check, response, (), variant["transport"])
            _, stats_wire_audit = checked(wire_check, stats_response, (), variant["transport"])
            _, resource_audit = checked(resource_check, stats_response["stats"], variant, budget, number + 1)
            require(stats_response["stats"]["plan_entries"] == stats_response["stats"]["result_entries"] == 0,
                    "admission unexpectedly prepared queries/results")
            record["opens"].append({"project": number, "request_wall_ms": wall_ms,
                                    "audit_ms": admission_audit + resource_audit + open_wire_audit + stats_wire_audit,
                                    "native": response, "stats_response": stats_response})
        record["initial_stats"] = record["opens"][-1]["stats_response"]["stats"]
        for index, key in enumerate(trace(scenario)):
            project, threshold = key
            metadata = suite["projects"][project]
            begin = time.perf_counter_ns()
            response, rows = client.run(metadata["name"], program(threshold), cache=item["policy"],
                                        emit="hex", max_rows=metadata["rows"],
                                        max_bytes=metadata["plaintext_bytes"])
            wall_ms = (time.perf_counter_ns() - begin) / 1e6
            stats_response = client.stats()
            verified, output_audit = checked(check_output, rows, oracle[key], response)
            _, row_wire_audit = checked(wire_check, response, verified["output_rows"], variant["transport"])
            _, stats_wire_audit = checked(wire_check, stats_response, (), variant["transport"])
            require(response["generation"] == metadata["generation"], "request generation differs")
            modeled, policy_audit = checked(model.observe, key, verified["output_rows"], response,
                                            stats_response["stats"], variant)
            record["requests"].append({"index": index, "project": project, "threshold": threshold,
                "request_wall_ms": wall_ms, "oracle_audit_ms": output_audit + policy_audit + row_wire_audit + stats_wire_audit,
                "native": response, "stats_response": stats_response,
                "verification": verified, "lru_model": modeled})
            del rows
        begin = time.perf_counter_ns()
        client.close()
        record["close_ms"] = (time.perf_counter_ns() - begin) / 1e6
        record["raw_session_wall_ms"] = (time.perf_counter_ns() - started) / 1e6
        record["oracle_audit_ms"] = audit_ns / 1e6
        record["service_ms"] = record["raw_session_wall_ms"] - record["oracle_audit_ms"]
        record["returncode"] = client.process.returncode
        record["close_result"] = client.close_result
        record["wire_bytes_received"] = client.wire_bytes_received
        record["wire_bytes_sent"] = client.wire_bytes_sent
        # Post-close consistency checks are outside the already stopped session clock.
        wire_check(record["close_result"], (), variant["transport"])
        record["wire_totals"] = reconcile_wire(record)
        require(record["returncode"] == 0 and record["service_ms"] > 0, "native exit or service time invalid")
        record["verified_rows"], record["verified_bytes"] = model.rows, model.bytes
        record["final_stats"] = record["requests"][-1]["stats_response"]["stats"]
        all_stats = [record["initial_stats"]] + [x["stats_response"]["stats"] for x in record["requests"]]
        record["peak_retained_worker_bytes"] = max(x["plan_worker_bytes"] for x in all_stats)
        record["peak_retained_worker_entries"] = max(x["worker_entries"] for x in all_stats)
        record["phase_sums_ms"] = {
            "admission_requests": sum(x["request_wall_ms"] for x in record["opens"]),
            "query_requests": sum(x["request_wall_ms"] for x in record["requests"]),
            "stats_requests": ready["client_wall_ms"] + sum(x["stats_response"]["client_wall_ms"] for x in record["opens"] + record["requests"]),
            "plan": sum(x["native"]["plan_ms"] for x in record["requests"]),
            "execution": sum(x["native"]["execution_ms"] for x in record["requests"]),
            "cache_build": sum(x["native"]["cache_build_ms"] for x in record["requests"]),
        }
        record.update(status="passed", finished_utc=utc_now())
    except BaseException as error:
        if client is not None:
            client.close(force=True)
        record.update(status="failed", error=str(error), traceback=traceback.format_exc(), finished_utc=utc_now())
        save(file, record, exclusive=True)
        raise
    save(file, record, exclusive=True)
    return {"path": file.relative_to(directory).as_posix(), "sha256": sha256(file), "schedule": item}


def verify_live(directory, suite):
    require(current_code(suite["binary"]) == suite["code_identity"], "live measured code/binary changed")
    require(input_inventory(directory) == suite["input_identity"], "copied experiment inputs changed")


def execute(args):
    require(args.repeats >= 10 and args.repeats % 10 == 0,
            "repeats must be a positive multiple of ten for scenario-position and transport-order balance")
    require(hasattr(os, "sched_getaffinity") and hasattr(os, "sched_setaffinity"),
            "run requires CPU-affinity APIs; portable audit/summarize do not")
    original_affinity = sorted(os.sched_getaffinity(0))
    require(bool(original_affinity), "no permitted CPU for the experiment")
    directory, suite, oracle, values = prepare(args)
    affinity = [original_affinity[0]]
    run = {"schema_version": 1, "status": "running", "started_utc": utc_now(),
           "suite_sha256": sha256(directory / "suite.json"), "records": [],
           "original_affinity": original_affinity, "measurement_affinity": affinity,
           "platform": platform.platform(), "python": sys.version, "processor": platform.processor(),
           "code_before": current_code(suite["binary"]), "inputs_before": input_inventory(directory),
           "schedule": suite["schedule"], "affinity_restored": False,
           "timing_contract": {
               "primary": "service_ms = spawn-through-close raw_session_wall_ms minus explicitly timed oracle/audit intervals",
               "included": "readiness, all twelve full admissions, queries, preparation, Capture, full JSONL/hex or SFC1/raw transfer and identical Python materialization, STATS after admissions and queries, shutdown, control bookkeeping and untimed assertions",
               "excluded": "one-time copied-evidence/source preparation, oracle loading, identity checks outside sessions, explicitly timed full-byte/descriptors/LRU/resource/wire audits, serialization of raw records after close",
               "observer_effect": "subtracting timed audit work does not reverse its cache, allocator or scheduling effects",
               "phase_diagnostics": "native phase clocks overlap client clocks; row framing is the controlled factor, not a separately isolated causal timer",
               "wire_scope": "bytes actually consumed from native stdout and written to native stdin, including STATS and QUIT; not physical I/O or RSS",
               "environment": "runner and children share one permitted logical CPU; no exclusive ownership or cache flushing",
               "statistics": "fresh-process paired observations by repetition; ranges are descriptive, not confidence intervals"}}
    save(directory / "RUN.json", run, exclusive=True)
    try:
        os.sched_setaffinity(0, affinity)
        require(sorted(os.sched_getaffinity(0)) == affinity, "runner affinity was not applied")
        previous = None
        for item in suite["schedule"]:
            group = item["repetition"], item["scenario"]
            if group != previous:
                verify_live(directory, suite)
                previous = group
            print(f"{item['ordinal'] + 1}/{len(suite['schedule'])} {item['scenario']} repetition {item['repetition']} {item['variant']['name']}", flush=True)
            run["records"].append(run_process(directory, suite, oracle, values, item, affinity))
            save(directory / "RUN.json", run)
        verify_live(directory, suite)
        run["code_after"] = current_code(suite["binary"])
        run["inputs_after"] = input_inventory(directory)
        run.update(status="passed", finished_utc=utc_now())
    except BaseException as error:
        run.update(status="failed", error=str(error), traceback=traceback.format_exc(), finished_utc=utc_now())
        raise
    finally:
        try:
            os.sched_setaffinity(0, original_affinity)
            run["affinity_restored"] = sorted(os.sched_getaffinity(0)) == original_affinity
            require(run["affinity_restored"], "runner affinity restoration failed")
        except BaseException as error:
            run.update(status="failed", affinity_restore_error=str(error))
            raise
        finally:
            save(directory / "RUN.json", run)
    summary = audit(directory)
    save(directory / "SUMMARY.json", summary, exclusive=True)
    print(json.dumps({"status": "passed", "output": str(directory), "counts": summary["totals"]}), flush=True)


def close_number(actual, expected, message):
    require(isinstance(actual, (int, float)) and math.isfinite(actual)
            and math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-8), message)


def audit_process(record, suite, oracle, values, expected_item):
    require(record["status"] == "passed" and record["schedule"] == expected_item, "process schedule/status differs")
    require(record["code_identity_sha256"] == suite["code_identity_sha256"], "process code identity differs")
    variant, scenario = expected_item["variant"], expected_item["scenario"]
    budget = suite["cache_budgets"][scenario]
    require(record["native_options"] == native_options(variant, budget) and record["cache_budget"] == budget,
            "native options or budget differs")
    require(record["native_argv"] == [suite["binary"], "serve", "--transport", variant["transport"],
                                      *native_options(variant, budget)],
            "native invocation does not match the selected transport and controlled options")
    require(record["affinity"] == record["child_affinity"] and len(record["affinity"]) == 1,
            "process affinity record differs")
    require(len(record["opens"]) == PROJECTS, "incomplete admission inventory")
    resource_check(record["startup_result"]["stats"], variant, budget, 0)
    wire_check(record["startup_result"], (), variant["transport"])
    for index, entry in enumerate(record["opens"]):
        require(entry["project"] == index, "admission order differs")
        open_check(entry["native"], suite["projects"][index])
        wire_check(entry["native"], (), variant["transport"])
        wire_check(entry["stats_response"], (), variant["transport"])
        resource_check(entry["stats_response"]["stats"], variant, budget, index + 1)
    require(record["initial_stats"] == record["opens"][-1]["stats_response"]["stats"], "initial stats differ")
    model = ResultModel(values, budget, expected_item["policy"])
    requests = trace(scenario)
    require(len(record["requests"]) == len(requests), "request inventory incomplete")
    for index, (entry, key) in enumerate(zip(record["requests"], requests)):
        require((entry["index"], entry["project"], entry["threshold"]) == (index, *key), "request order/identity differs")
        wanted = describe_rows(oracle[key])
        verification, native = entry["verification"], entry["native"]
        require(verification["output_rows"] == wanted and verification["rows"] == len(wanted)
                and verification["bytes"] == sum(x["bytes"] for x in wanted)
                and verification["comparison"] == "row_order_path_length_and_every_original_byte",
                "recorded descriptors differ from independently read original bytes")
        require(native["status"] == "ok" and native["selected_rows"] == verification["rows"]
                and native["output_bytes"] == verification["bytes"]
                and native["generation"] == suite["projects"][key[0]]["generation"],
                "recorded native output identity/counters differ")
        wire_check(native, wanted, variant["transport"])
        wire_check(entry["stats_response"], (), variant["transport"])
        expected_model = model.observe(key, wanted, native, entry["stats_response"]["stats"], variant)
        require(entry["lru_model"] == expected_model, "saved LRU audit differs from recomputed model")
    require(record["returncode"] == 0 and record["verified_rows"] == model.rows
            and record["verified_bytes"] == model.bytes, "process exit/comparison totals differ")
    require(record["final_stats"] == record["requests"][-1]["stats_response"]["stats"], "final stats differ")
    audit_ms = record["startup_audit_ms"] + sum(x["audit_ms"] for x in record["opens"]) + sum(x["oracle_audit_ms"] for x in record["requests"])
    close_number(record["oracle_audit_ms"], audit_ms, "oracle/audit time sum differs")
    close_number(record["service_ms"], record["raw_session_wall_ms"] - audit_ms, "service subtraction differs")
    require(record["service_ms"] > 0 and all(x["oracle_audit_ms"] >= 0 for x in record["requests"]),
            "invalid recorded timing")
    expected_phases = {
        "admission_requests": sum(x["request_wall_ms"] for x in record["opens"]),
        "query_requests": sum(x["request_wall_ms"] for x in record["requests"]),
        "stats_requests": record["startup_result"]["client_wall_ms"] + sum(x["stats_response"]["client_wall_ms"] for x in record["opens"] + record["requests"]),
        "plan": sum(x["native"]["plan_ms"] for x in record["requests"]),
        "execution": sum(x["native"]["execution_ms"] for x in record["requests"]),
        "cache_build": sum(x["native"]["cache_build_ms"] for x in record["requests"]),
    }
    for name, expected in expected_phases.items():
        close_number(record["phase_sums_ms"][name], expected, "phase sum differs: " + name)
    stats = [record["initial_stats"]] + [x["stats_response"]["stats"] for x in record["requests"]]
    require(record["peak_retained_worker_bytes"] == max(x["plan_worker_bytes"] for x in stats)
            and record["peak_retained_worker_entries"] == max(x["worker_entries"] for x in stats),
            "observed worker peaks differ")
    wire_check(record["close_result"], (), variant["transport"])
    require(record["wire_totals"] == reconcile_wire(record), "wire totals differ from response accounting")
    return {"requests": model.runs, "verified_rows": model.rows, "verified_bytes": model.bytes,
            "wire_bytes_received": record["wire_bytes_received"], "wire_bytes_sent": record["wire_bytes_sent"],
            "result_hits": model.hits, "result_admissions": model.admissions,
            "result_evictions": model.evictions, "multi_victim_admissions": model.multivictim}


def distribution(values):
    require(values and all(isinstance(x, (int, float)) and math.isfinite(x) and x >= 0 for x in values),
            "invalid descriptive observations")
    return {"values": values, "minimum": min(values), "median": statistics.median(values),
            "maximum": max(values), "mean": statistics.mean(values)}


def ratios(numerator, denominator, definition):
    require(len(numerator) == len(denominator) and all(x > 0 for x in denominator), "invalid paired denominators")
    values = [a / b for a, b in zip(numerator, denominator)]
    require(all(x > 0 and math.isfinite(x) for x in values), "nonpositive paired ratio")
    return {"definition": definition, **distribution(values),
            "geometric_mean": math.exp(sum(math.log(x) for x in values) / len(values)),
            "pairs_above_one": sum(x > 1 for x in values), "pairs_equal_one": sum(x == 1 for x in values)}


def audit(directory):
    """Portable recorded-evidence audit; no native or historical external reads."""
    directory = Path(directory).resolve()
    suite_path = relative_file(directory, "suite.json")
    run_path = relative_file(directory, "RUN.json")
    source_suite_path = relative_file(directory, "EVIDENCE_SOURCE_SUITE.json")
    source_run_path = relative_file(directory, "EVIDENCE_SOURCE_RUN.json")
    build_path = relative_file(directory, "MEASURED_BUILD_MANIFEST.json")
    suite, run = load(suite_path), load(run_path)
    require(suite["status"] == "prepared" and run["status"] == "passed" and run["affinity_restored"],
            "experiment is incomplete/failed or affinity was not restored")
    require(sha256(suite_path) == run["suite_sha256"], "suite identity changed")
    require(suite["limits"] == LIMITS and suite["variants"] == list(VARIANTS)
            and suite["result_row_descriptor_bytes"] == 24, "frozen design constants differ")
    require(suite["repeats"] >= 10 and suite["repeats"] % 10 == 0, "unbalanced repetition count")
    require(suite["schedule"] == run["schedule"] == schedule(suite["repeats"]), "balanced schedule differs")
    require(suite["traces"] == {name: [list(k) for k in trace(name)] for name in SCENARIOS}, "trace definition differs")
    require(run["code_before"] == run["code_after"] == suite["code_identity"], "measured code identities differ")
    require(object_hash(suite["code_identity"]) == suite["code_identity_sha256"], "code identity digest differs")
    actual_inputs = input_inventory(directory)
    require(actual_inputs == suite["input_identity"] == run["inputs_before"] == run["inputs_after"]
            == suite["source_evidence"]["inputs"], "input identities differ")
    require(sha256(source_suite_path) == suite["source_evidence"]["suite_sha256"]
            and sha256(source_run_path) == suite["source_evidence"]["run_sha256"],
            "source-experiment provenance copies differ")
    source = load(source_suite_path)
    require(source["source_files"] == suite["source_files"] and source["projects"] == suite["projects"]
            and source["input_identity"] == actual_inputs, "copied original corpus differs from provenance")
    build = load(build_path)
    require(sha256(build_path) == suite["code_identity"]["build_manifest"]["sha256"]
            and build["status"] == "passed" and build["input_stability"] == "unchanged"
            and build["inputs_before"] == build["inputs_after"], "frozen build manifest differs or failed")
    require(build["inputs_after"]["project"] == suite["code_identity"]["project"], "compiled-input inventory differs")
    require(build["binaries"]["suffice_catalog"]["sha256"] == suite["code_identity"]["binary"]["sha256"],
            "recorded binary identity differs from build")
    frozen = dict(suite["code_identity"]["project"])
    frozen.update(suite["code_identity"]["python"])
    frozen.update(suite["code_identity"]["methods"])
    require(frozen == suite["frozen_source_hashes"], "frozen source inventory differs")
    source_root = directory / "measurement_source"
    require(source_root.is_dir() and not source_root.is_symlink(), "missing ordinary frozen-source directory")
    actual_frozen = {}
    for path in sorted(source_root.rglob("*")):
        require(not path.is_symlink(), "frozen source contains a symbolic link")
        if path.is_dir():
            continue
        require(path.is_file(), "frozen source contains a nonregular object")
        relative = path.relative_to(source_root).as_posix()
        actual_frozen[relative] = sha256(relative_file(source_root, relative))
    require(actual_frozen == frozen, "frozen measured source inventory or bytes changed")
    oracle = original_oracle(directory, suite["source_files"])
    values = capacities(oracle)
    require(budgets(values) == suite["cache_budgets"], "independent capacity budgets differ")
    expected_queries = [{"project": key[0], "threshold": key[1], "capacity": values[key],
                         "output_rows": describe_rows(rows)} for key, rows in sorted(oracle.items())]
    require(expected_queries == suite["query_results"], "query descriptors differ from original-body oracle")
    require(len(run["records"]) == len(suite["schedule"]), "process record inventory incomplete")
    expected_files = {item["path"] for item in run["records"]}
    actual_files = {p.relative_to(directory).as_posix() for p in (directory / "processes").glob("*.json")}
    require(actual_files == expected_files and len(expected_files) == len(run["records"]),
            "process files contain missing, duplicate or extra records")
    records, audits = {}, []
    totals = {"processes": 0, "complete_output_checks": 0, "verified_rows": 0, "verified_bytes": 0,
              "result_hits": 0, "result_admissions": 0, "result_evictions": 0,
              "wire_bytes_received": 0, "wire_bytes_sent": 0}
    for item, expected_item in zip(run["records"], suite["schedule"]):
        file = relative_file(directory, item["path"])
        require(sha256(file) == item["sha256"] and item["schedule"] == expected_item, "raw process record changed")
        record = load(file)
        require(record["affinity"] == run["measurement_affinity"], "process CPU differs from run")
        checked = audit_process(record, suite, oracle, values, expected_item)
        key = expected_item["scenario"], expected_item["repetition"], expected_item["variant"]["name"]
        require(key not in records, "duplicate process observation")
        records[key] = record
        audits.append({"path": item["path"], **checked})
        totals["processes"] += 1
        totals["complete_output_checks"] += checked["requests"]
        for name in ("verified_rows", "verified_bytes", "result_hits", "result_admissions", "result_evictions", "wire_bytes_received", "wire_bytes_sent"):
            totals[name] += checked[name]
    summary = {"schema_version": 1, "status": "passed", "experiment": "transport_v0_13",
               "scope": "Local retained-record audit; original run compared full output bytes. No native outputs/timings re-executed by audit.",
               "suite_sha256": sha256(suite_path), "run_sha256": sha256(run_path),
               "totals": totals, "corpus_files": len(suite["source_files"]), "input_files": len(actual_inputs),
               "frozen_source_files": len(frozen), "repeats": suite["repeats"], "process_audits": audits,
               "scenarios": {}, "limitations": ["One corpus partitioned into twelve logical projects", "Three body-length predicates plus an empty-output control",
                    "One Linux configuration and shared pinned CPU", "No cache flushing or exclusive CPU ownership",
                    "Pipe wire bytes are not physical I/O or resident memory", "Phase timings include instrumentation and do not isolate causal costs",
                    "Descriptive paired observations, not independent per-request trials or confidence intervals"]}
    for scenario in SCENARIOS:
        scenario_summary = {"requests_per_process": len(trace(scenario)), "cache_budget": suite["cache_budgets"][scenario],
                            "variants": {}, "paired_comparisons": {}}
        def series(name, field):
            return [records[scenario, r, name][field] for r in range(suite["repeats"])]
        for variant in VARIANTS:
            name = variant["name"]
            stats = [records[scenario, r, name]["final_stats"] for r in range(suite["repeats"])]
            entry = {field: distribution(series(name, field)) for field in
                     ("raw_session_wall_ms", "oracle_audit_ms", "service_ms", "peak_retained_worker_bytes",
                      "peak_retained_worker_entries", "wire_bytes_received", "wire_bytes_sent")}
            entry["final_stats"] = {field: distribution([s[field] for s in stats]) for field in stats[0]}
            entry["phase_sums_ms"] = {field: distribution([records[scenario, r, name]["phase_sums_ms"][field]
                                                          for r in range(suite["repeats"])])
                                      for field in records[scenario, 0, name]["phase_sums_ms"]}
            entry["wire_totals"] = {field: distribution([records[scenario, r, name]["wire_totals"][field]
                                                        for r in range(suite["repeats"])])
                                    for field in records[scenario, 0, name]["wire_totals"]}
            scenario_summary["variants"][name] = entry
        comparisons = scenario_summary["paired_comparisons"]
        for field, label in (("service_ms", "service"), ("raw_session_wall_ms", "raw_wall"),
                             ("wire_bytes_received", "response_bytes")):
            comparisons["jsonl_over_binary_" + label] = ratios(series("jsonl", field), series("binary", field),
                "JSONL / Binary " + label + " within repetition; above one favors Binary")
        comparisons["jsonl_over_binary_query_ms"] = ratios(
            [records[scenario, r, "jsonl"]["phase_sums_ms"]["query_requests"] for r in range(suite["repeats"])],
            [records[scenario, r, "binary"]["phase_sums_ms"]["query_requests"] for r in range(suite["repeats"])],
            "JSONL / Binary sum of complete query-request times; excludes startup/admission/stats/close, not the primary service metric")
        reductions = [1 - records[scenario, r, "binary"]["wire_bytes_received"] / records[scenario, r, "jsonl"]["wire_bytes_received"]
                      for r in range(suite["repeats"])]
        require(all(math.isfinite(x) for x in reductions), "nonfinite wire reduction")
        comparisons["response_byte_reduction"] = {
            "definition": "1 - Binary/JSONL measured complete-session stdout bytes; negative means more bytes",
            "values": reductions, "minimum": min(reductions), "median": statistics.median(reductions),
            "maximum": max(reductions), "mean": statistics.mean(reductions)}
        scenario_summary["matched_final_stats"] = all(
            records[scenario, r, "jsonl"]["final_stats"] == records[scenario, r, "binary"]["final_stats"]
            for r in range(suite["repeats"]))
        require(scenario_summary["matched_final_stats"], "transport unexpectedly changed runtime policy counters or resource state")
        scenario_summary["matched_request_wire_bytes"] = all(
            records[scenario, r, "jsonl"]["wire_bytes_sent"] == records[scenario, r, "binary"]["wire_bytes_sent"]
            for r in range(suite["repeats"]))
        require(scenario_summary["matched_request_wire_bytes"], "matched transports sent different request byte counts")
        summary["scenarios"][scenario] = scenario_summary

    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="run", choices=("run", "audit", "summarize"))
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--evidence-source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=10)
    args = parser.parse_args()
    if args.command == "run":
        if args.binary is None or args.evidence_source is None:
            parser.error("run requires --binary and --evidence-source")
        execute(args)
    else:
        summary = audit(args.output)
        existing = args.output / "SUMMARY.json"
        if existing.exists():
            require(load(relative_file(args.output, "SUMMARY.json")) == summary,
                    "saved summary differs from independent recomputation")
        elif args.command == "summarize":
            save(existing, summary, exclusive=True)
        print(json.dumps({"status": "passed", "mode": args.command, "counts": summary["totals"],
                          "historical_native_execution": False}, indent=2))


if __name__ == "__main__":
    main()
