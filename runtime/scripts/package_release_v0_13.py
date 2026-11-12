#!/usr/bin/env python3
"""Package immutable 0.13 measurements and final gates; perform no native reruns."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION_ROOT = "Suffice_0_13_0"
BASELINE_SHA = "da69f1f305452e1cffd89ddd50aea1009ba43ab1f7eee3e52624c2b60a170ec8"
SOURCE_DIRS = ("src", "include", "apps", "scripts", "tests", "examples", "docs", "research", "figures", "baseline")
ROOT_FILES = ("CMakeLists.txt", "LICENSE", "README.md", "REPORT.md", "PROJECT_HANDOFF.md",
              "RESEARCH_DOSSIER.md", "RELEASE_CHECKLIST.md", "VALIDATION_SUMMARY.json")
RECEIPT_DIRS = ("producer", "client_framing", "worker_jsonl", "worker_binary",
                "integration_release_jsonl", "integration_release_binary",
                "integration_sanitize_jsonl", "integration_sanitize_binary",
                "transport_integration_release", "transport_integration_sanitize")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True)+"\n", encoding="utf-8")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def copy_file(source, destination):
    require(source.is_file() and not source.is_symlink(), f"Expected regular file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_tree(relative, stage):
    folder = ROOT / relative
    require(folder.is_dir() and not folder.is_symlink(), f"Expected directory: {folder}")
    for source in sorted(folder.rglob("*")):
        require(not source.is_symlink(), f"Symlink in selected evidence: {source}")
        if "__pycache__" in source.parts or source.suffix in (".pyc", ".pyo"):
            continue
        if source.is_file():
            copy_file(source, stage/source.relative_to(ROOT))


def checked_build(tag, stage):
    relative = Path("build") / ("transport_"+tag)
    manifest = load(ROOT/relative/"build_manifest.json")
    require(manifest["status"] == "passed" and manifest["input_stability"] == "unchanged", f"Build not passed: {tag}")
    require(manifest["inputs_before"] == manifest["inputs_after"], f"Build identities differ: {tag}")
    for name, digest in manifest["inputs_before"]["project"].items():
        require(sha(ROOT/name) == digest, f"Compiled source changed: {name}")
    for check in [*manifest["commands"], *manifest["tests"].values()]:
        require(check["status"] == "passed" and sha(check["log"]) == check["log_sha256"], f"Build command/check mismatch: {tag}")
    for name, info in manifest["binaries"].items():
        require(sha(info["path"]) == info["sha256"], f"Built executable differs: {tag}/{name}")
    support = manifest["support"]
    for name, digest in support["identity"]["inputs"].items():
        require(sha(ROOT/name) == digest, f"Core-cache input differs: {name}")
    require(sha(support["library"]) == support["library_sha256"], f"Core archive differs: {tag}")
    cached_manifest_path = Path(support["library"]).parent/"manifest.json"
    cached_manifest = load(cached_manifest_path)
    require(cached_manifest["identity"] == support["identity"] and cached_manifest["library_sha256"] == support["library_sha256"], f"Core provenance differs: {tag}")
    copy_file(ROOT/relative/"build_manifest.json", stage/relative/"build_manifest.json")
    copy_tree(relative/"logs", stage)
    copy_file(cached_manifest_path, stage/"validation"/("core_cache_"+tag+"_manifest.json"))
    return {"status":"passed", "project_inputs_checked":len(manifest["inputs_before"]["project"]),
            "built_binaries_checked":len(manifest["binaries"]), "executable_checks":len(manifest["tests"]),
            "commands_checked":len(manifest["commands"]), "support_cache_hit":support["cache_hit"],
            "support_inputs_checked":len(support["identity"]["inputs"]),
            "support_cache_key":support["cache_key"], "support_archive_sha256":support["library_sha256"],
            "new_support_compilation_ms":support["this_invocation_build_ms"],
            "scope":"Direct builder evidence; implicit system dependencies are not fully hashed; CMake and Windows not executed"}


def validate_receipts():
    receipts = {}
    for folder in RECEIPT_DIRS:
        path = ROOT/"results"/folder/"RESULT.json"
        result = load(path)
        require(result["status"] == "passed", f"Gate failed: {folder}")
        for key in ("failures", "errors", "skipped"):
            require(result.get(key, 0) == 0, f"Unsuccessful cases: {folder}/{key}")
        if "inputs_before" in result:
            require(result["inputs_before"] == result["inputs_after"] and result["inputs_stable"], f"Integration identities changed: {folder}")
            for absolute, digest in result["inputs_before"].items():
                require(sha(absolute) == digest, f"Integration input now differs: {absolute}")
        bindings = {"client_sha256":"scripts/catalog_client.py"}
        if folder == "client_framing":
            bindings["test_sha256"] = "tests/test_catalog_framing.py"
        elif folder == "producer":
            bindings.update({"producer_sha256":"scripts/artifact_catalog.py", "test_sha256":"tests/test_artifact_catalog.py"})
            require(sha(result["packer"]) == result["packer_sha256"], "Producer packer differs")
        elif folder.startswith("worker_"):
            bindings.update({"test_sha256":"tests/test_worker_protocol.py", "binary_sha256":"build/transport_release/suffice_catalog"})
        for key, relative in bindings.items():
            if key in result:
                require(sha(ROOT/relative) == result[key], f"Receipt binding differs: {folder}/{key}")
        receipts[folder] = {"path":path.relative_to(ROOT).as_posix(), "sha256":sha(path),
                            "status":result["status"], "counters":result.get("counters", {}),
                            "test_cases":result.get("test_cases"), "groups":result.get("groups"),
                            "complete_output_checks":result.get("complete_output_checks")}
    native = load(ROOT/"research/native_wire_check.json")
    for name, digest in native["inputs"].items():
        require(sha(ROOT/name) == digest, f"Native wire input differs: {name}")
    return receipts, native["test_result"]


def changes_from_baseline(stage):
    baseline = stage/"baseline/Suffice_0_12_0.zip"
    require(sha(baseline) == BASELINE_SHA, "Baseline identity changed")
    old = {}
    with zipfile.ZipFile(baseline) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            root, sep, relative = entry.filename.partition("/")
            require(sep and root == "Suffice_0_12_0", "Unexpected baseline archive root")
            if relative.split("/", 1)[0] in SOURCE_DIRS[:6] or relative in ("CMakeLists.txt", "LICENSE"):
                old[relative] = hashlib.sha256(archive.read(entry)).hexdigest()
    current = {}
    for folder in SOURCE_DIRS[:6]:
        for path in (stage/folder).rglob("*"):
            if path.is_file():
                current[path.relative_to(stage).as_posix()] = sha(path)
    for name in ("CMakeLists.txt", "LICENSE"):
        current[name] = sha(stage/name)
    removed = sorted(set(old)-set(current))
    require(not removed, f"Baseline code omitted: {removed}")
    return {"scope":"Active source/scripts/tests/examples plus CMakeLists.txt and LICENSE; documents/evidence separately inventoried",
            "baseline_archive_sha256":BASELINE_SHA,
            "unchanged_files":sum(old.get(k) == v for k,v in current.items()),
            "modified":[{"path":k,"before_sha256":old[k],"after_sha256":v} for k,v in sorted(current.items()) if k in old and old[k] != v],
            "added":[{"path":k,"sha256":v} for k,v in sorted(current.items()) if k not in old], "removed":removed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    require(not output.is_relative_to(ROOT), "Release output must be outside the source tree to prevent recursive staging")
    stage = output/VERSION_ROOT
    archive_path = output/(VERSION_ROOT+".zip")
    require(not stage.exists() and not archive_path.exists(), "Refusing to replace a staged release or archive")
    require(not (output/"RELEASE_RECEIPT.json").exists(), "Refusing to replace an existing release receipt")
    output.mkdir(parents=True, exist_ok=True)
    stage.mkdir()
    receipts, wire = validate_receipts()
    builds = {tag:checked_build(tag, stage) for tag in ("release", "sanitize")}
    summary_path = ROOT/"results/transport_v0_13/SUMMARY.json"
    summary = load(summary_path)
    require(summary["status"] == "passed", "Transport experiment did not pass")
    reviews = {}
    for name in ("results_review_v0_13", "release_validation_review_v0_13", "report_review_v0_13"):
        path = ROOT/"research"/(name+".json")
        require(path.is_file(), f"Missing independent review: {name}")
        review = load(path)
        allowed_statuses = {"passed", "passed_with_scope_limits"} if name == "release_validation_review_v0_13" else {"passed"}
        require(review.get("status") in allowed_statuses, f"Independent review has not passed: {name}")
        for relative, digest in review.get("reviewed_sha256", {}).items():
            require(sha(ROOT/relative) == digest, f"Reviewed artifact changed: {relative}")
        reviews[name] = {"path":path.relative_to(ROOT).as_posix(),"sha256":sha(path),"status":review["status"]}
    validation = {"schema_version":1,"status":"passed","release":"0.13.0",
                  "scope":"Final executed gate receipts, current input/binary checks and retained experiment; no tests rerun for this summary",
                  "native_windows_tested":False, "cmake_route_executed":False,
                  "builds":builds,"native_wire":wire,"gates":receipts,"independent_reviews":reviews,
                  "experiment":{"summary_sha256":sha(summary_path),"totals":summary["totals"],
                                "repeats":summary["repeats"],"frozen_source_files":summary["frozen_source_files"],"input_files":summary["input_files"]},
                  "limitations":["Overlapping gate counts are not independent performance trials", "Sanitizer leak detection disabled; installed codec not instrumented", "Compiler/default system headers and runtime dependencies are not a fully hermetic toolchain", "Core archives reused under verified source/compiler identities; new support compilation was zero", "Portable retained-evidence audit and archive closure are recorded separately"]}
    write_json(ROOT/"VALIDATION_SUMMARY.json",validation)
    for name in ROOT_FILES:
        copy_file(ROOT/name, stage/name)
    for folder in SOURCE_DIRS:
        copy_tree(folder, stage)
    copy_tree("results/transport_v0_13", stage)
    for name in ("transport_run.log", "build_release.log", "build_sanitize.log"):
        copy_file(ROOT/"results"/name, stage/"results"/name)
    for folder in RECEIPT_DIRS:
        for source in sorted((ROOT/"results"/folder).iterdir()):
            if source.is_file() and source.suffix in (".json", ".jsonl", ".log"):
                copy_file(source,stage/source.relative_to(ROOT))
    for name in ("suffice", "suffice_catalog"):
        relative=Path("build/transport_release")/name
        copy_file(ROOT/relative, stage/relative)
    changes=changes_from_baseline(stage)
    write_json(stage/"CHANGES_FROM_0_12.json",changes)

    # Relocation checks package closure without rerunning any native historical work.
    command=[sys.executable,str(stage/"scripts/benchmark_transport_v0_13.py"),"audit","--output",str(stage/"results/transport_v0_13")]
    checked=subprocess.run(command,cwd=stage,capture_output=True,text=True,check=True,
                           env=dict(os.environ,PYTHONDONTWRITEBYTECODE="1"))
    audit=json.loads(checked.stdout)
    require(audit["status"] == "passed" and not audit["historical_native_execution"], "Relocated portable audit failed")
    write_json(stage/"validation/PACKAGED_EVIDENCE_AUDIT.json",audit)

    files={}
    for path in sorted(stage.rglob("*")):
        require(not path.is_symlink(),f"Staged symlink: {path}")
        if path.is_file():
            files[path.relative_to(stage).as_posix()]={"bytes":path.stat().st_size,"sha256":sha(path)}
    inventory={"schema_version":1,"release":"0.13.0","archive_root":VERSION_ROOT,
               "scope":"Every payload file except RELEASE_INVENTORY.json; external receipt hashes inventory and archive",
               "build_source_checks":builds,"packaged_evidence_audit":audit,
               "included_native_binaries":["build/transport_release/suffice","build/transport_release/suffice_catalog"],
               "omitted_reproducible_work":["object files and static core archives", "sanitizer executables", "other Release test/benchmark executables", "transient corruption/byte fixtures", "interpreter caches", "superseded pre-final client receipts"],
               "file_count":len(files),"uncompressed_bytes":sum(v["bytes"] for v in files.values()),"files":files}
    write_json(stage/"RELEASE_INVENTORY.json",inventory)
    with zipfile.ZipFile(archive_path,"x",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if not path.is_file():
                continue
            entry=zipfile.ZipInfo(VERSION_ROOT+"/"+path.relative_to(stage).as_posix(),date_time=(2026,11,12,0,0,0))
            entry.create_system=3
            executable=bool(path.stat().st_mode & stat.S_IXUSR)
            entry.external_attr=(stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            archive.writestr(entry,path.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
    with zipfile.ZipFile(archive_path) as archive:
        require(archive.testzip() is None, "ZIP CRC validation failed")
        names=[x.filename for x in archive.infolist()]
        require(len(names) == len(set(names)) == len(files)+1,"ZIP count mismatch")
        for name,identity in files.items():
            payload=archive.read(VERSION_ROOT+"/"+name)
            require(len(payload) == identity["bytes"] and hashlib.sha256(payload).hexdigest() == identity["sha256"],f"ZIP identity mismatch: {name}")
        require(archive.read(VERSION_ROOT+"/RELEASE_INVENTORY.json") == (stage/"RELEASE_INVENTORY.json").read_bytes(),"ZIP inventory differs")
    receipt={"schema_version":1,"status":"passed","release":"0.13.0",
             "created_utc":datetime.now(timezone.utc).isoformat(),"archive":archive_path.name,
             "archive_bytes":archive_path.stat().st_size,"archive_sha256":sha(archive_path),
             "zip_crc_verified":True,"all_payload_hashes_verified":True,"zip_file_count":len(files)+1,
             "inventory_sha256":sha(stage/"RELEASE_INVENTORY.json"),
             "baseline_archive_sha256":BASELINE_SHA,"relocated_portable_audit":audit,
             "build_source_checks":builds,"changes":{"unchanged":changes["unchanged_files"],"modified":len(changes["modified"]),"added":len(changes["added"]),"removed":len(changes["removed"])},
             "native_tests_or_measurements_reexecuted_during_packaging":False}
    write_json(output/"RELEASE_RECEIPT.json",receipt)
    copy_file(stage/"RELEASE_INVENTORY.json",ROOT/"RELEASE_INVENTORY.json")
    print(json.dumps(receipt,indent=2))


if __name__ == "__main__":
    main()
