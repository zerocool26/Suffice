#!/usr/bin/env python3
"""Package the completed 0.12 evidence; preserve measured bytes and verify the ZIP."""
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
VERSION_ROOT = "Suffice_0_12_0"
SOURCE_DIRS = ("src", "include", "apps", "scripts", "tests", "examples", "docs", "research", "figures", "baseline")
ROOT_FILES = ("CMakeLists.txt", "LICENSE", "README.md", "REPORT.md", "PROJECT_HANDOFF.md",
              "RESEARCH_DOSSIER.md", "VALIDATION_SUMMARY.json")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def copy_file(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"Expected regular nonsymlink file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_tree(relative, stage):
    folder = ROOT / relative
    if folder.is_symlink() or not folder.is_dir():
        raise ValueError(f"Expected nonsymlink directory: {folder}")
    for source in sorted(folder.rglob("*")):
        if source.is_symlink():
            raise ValueError(f"Symlink in selected evidence: {source}")
        if "__pycache__" in source.parts or source.suffix in (".pyc", ".pyo"):
            continue
        if source.is_file():
            copy_file(source, stage / source.relative_to(ROOT))


def checked_build(tag, stage):
    relative = Path("build") / ("retention_" + tag)
    manifest = load(ROOT / relative / "build_manifest.json")
    if manifest["status"] != "passed" or manifest["input_stability"] != "unchanged":
        raise ValueError(f"Build not ready: {tag}")
    for name, digest in manifest["inputs_before"]["project"].items():
        if sha(ROOT / name) != digest:
            raise ValueError(f"Compiled source changed: {name}")
    for name, check in manifest["tests"].items():
        if check["status"] != "passed" or sha(check["log"]) != check["log_sha256"]:
            raise ValueError(f"Build check evidence mismatch: {tag}/{name}")
    copy_file(ROOT / relative / "build_manifest.json", stage / relative / "build_manifest.json")
    copy_tree(relative / "logs", stage)
    support = stage / "validation" / ("support_logs_" + tag)
    support.mkdir(parents=True)
    mapping = []
    for command in manifest["commands"]:
        if "codegen_cache" not in command.get("log", ""):
            continue
        source = Path(command["log"])
        if sha(source) != command["log_sha256"]:
            raise ValueError(f"Support log differs: {source}")
        dest = support / source.name
        if dest.exists():
            raise ValueError(f"Ambiguous support-log basename: {dest}")
        copy_file(source, dest)
        mapping.append({"original_recorded_path": str(source), "packaged_path": dest.relative_to(stage).as_posix(),
                        "sha256": sha(dest)})
    write_json(support / "INDEX.json", {"scope": "Relocated support-compiler/archive logs only; no code-generation objects or libraries.",
                                       "files": mapping})
    return {"status": "passed", "project_inputs_checked": len(manifest["inputs_before"]["project"]),
            "checks": len(manifest["tests"]), "support_logs": len(mapping)}


def changes_from_baseline(stage):
    baseline = stage / "baseline/Suffice_0_11_0_Code.zip"
    old = {}
    with zipfile.ZipFile(baseline) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            _, separator, relative = entry.filename.partition("/")
            if not separator:
                raise ValueError("Baseline lacks its expected project root")
            if relative.split("/", 1)[0] in SOURCE_DIRS[:6] or relative in ("CMakeLists.txt", "LICENSE"):
                old[relative] = hashlib.sha256(archive.read(entry)).hexdigest()
    current = {}
    for folder in SOURCE_DIRS[:6]:
        for path in (stage / folder).rglob("*"):
            if path.is_file():
                current[path.relative_to(stage).as_posix()] = sha(path)
    for name in ("CMakeLists.txt", "LICENSE"):
        current[name] = sha(stage / name)
    removed = sorted(set(old) - set(current))
    if removed:
        raise ValueError(f"Baseline code was omitted: {removed}")
    return {"scope": "Active src/include/apps/scripts/tests/examples plus CMakeLists.txt and LICENSE; documentation and evidence are inventoried separately.",
            "baseline_archive_sha256": sha(baseline),
            "unchanged_files": sum(old.get(k) == v for k, v in current.items()),
            "modified": [{"path": k, "before_sha256": old[k], "after_sha256": v}
                         for k, v in sorted(current.items()) if k in old and old[k] != v],
            "added": [{"path": k, "sha256": v} for k, v in sorted(current.items()) if k not in old],
            "removed": removed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stage = output / VERSION_ROOT
    archive_path = output / (VERSION_ROOT + ".zip")
    if stage.exists() or archive_path.exists():
        raise ValueError("Refusing to replace an existing staged release or archive")
    stage.mkdir()
    for name in ROOT_FILES:
        copy_file(ROOT / name, stage / name)
    for folder in SOURCE_DIRS:
        copy_tree(folder, stage)
    copy_tree("results/retention_v0_12", stage)
    copy_file(ROOT / "results/retention_run.log", stage / "results/retention_run.log")
    for folder in ("producer", "integration_release", "integration_sanitize", "worker_protocol"):
        for source in sorted((ROOT / "results" / folder).iterdir()):
            if source.is_file() and source.suffix in (".json", ".jsonl", ".log"):
                copy_file(source, stage / source.relative_to(ROOT))
    builds = {tag: checked_build(tag, stage) for tag in ("release", "sanitize")}
    for name in ("suffice", "suffice_catalog"):
        relative = Path("build/retention_release") / name
        copy_file(ROOT / relative, stage / relative)
    write_json(stage / "CHANGES_FROM_0_11.json", changes_from_baseline(stage))

    # This resolves relocated evidence and catches omissions before archiving.
    # It invokes the portable auditor only; no native measurement is repeated.
    command = [sys.executable, str(stage / "scripts/benchmark_retention_v0_12.py"),
               "audit", "--output", str(stage / "results/retention_v0_12")]
    audit_environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    checked = subprocess.run(command, cwd=stage, capture_output=True, text=True, check=True,
                             env=audit_environment)
    audit = json.loads(checked.stdout)
    if audit["status"] != "passed" or audit["historical_native_execution"]:
        raise ValueError("Relocated portable audit did not pass")
    write_json(stage / "validation/PACKAGED_EVIDENCE_AUDIT.json", audit)

    files = {}
    for path in sorted(stage.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Staged symlink: {path}")
        if path.is_file():
            files[path.relative_to(stage).as_posix()] = {"bytes": path.stat().st_size, "sha256": sha(path)}
    inventory = {"schema_version": 1, "release": "0.12.0", "archive_root": VERSION_ROOT,
                 "scope": "Every payload file except RELEASE_INVENTORY.json itself; the archive receipt hashes that inventory.",
                 "build_source_checks": builds,
                 "packaged_evidence_audit": audit,
                 "included_native_binaries": ["build/retention_release/suffice", "build/retention_release/suffice_catalog"],
                 "omitted_reproducible_work": ["compiled objects and static support libraries", "sanitizer binaries", "other Release test/benchmark executables", "temporary corruption fixtures", "interpreter caches"],
                 "file_count": len(files), "uncompressed_bytes": sum(x["bytes"] for x in files.values()), "files": files}
    write_json(stage / "RELEASE_INVENTORY.json", inventory)

    # Stable metadata supports inspection; matching ZIP bytes across different
    # Python/zlib versions is not a reproduction requirement.
    with zipfile.ZipFile(archive_path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(stage).as_posix()
            entry = zipfile.ZipInfo(VERSION_ROOT + "/" + relative, date_time=(2026, 11, 12, 0, 0, 0))
            entry.create_system = 3
            executable = bool(path.stat().st_mode & stat.S_IXUSR)
            entry.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    with zipfile.ZipFile(archive_path) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP CRC validation failed")
        names = [entry.filename for entry in archive.infolist()]
        if len(names) != len(set(names)) or len(names) != len(files) + 1:
            raise ValueError("ZIP inventory count mismatch")
        for name, identity in files.items():
            payload = archive.read(VERSION_ROOT + "/" + name)
            if len(payload) != identity["bytes"] or hashlib.sha256(payload).hexdigest() != identity["sha256"]:
                raise ValueError(f"ZIP payload identity mismatch: {name}")
        if archive.read(VERSION_ROOT + "/RELEASE_INVENTORY.json") != (stage / "RELEASE_INVENTORY.json").read_bytes():
            raise ValueError("ZIP inventory bytes differ")
    receipt = {"schema_version": 1, "status": "passed", "release": "0.12.0",
               "created_utc": datetime.now(timezone.utc).isoformat(),
               "archive": archive_path.name, "archive_bytes": archive_path.stat().st_size,
               "archive_sha256": sha(archive_path), "zip_crc_verified": True,
               "all_payload_hashes_verified": True, "zip_file_count": len(files) + 1,
               "inventory_sha256": sha(stage / "RELEASE_INVENTORY.json"),
               "relocated_portable_audit": audit, "build_source_checks": builds,
               "native_tests_or_measurements_reexecuted_during_packaging": False}
    write_json(output / "RELEASE_RECEIPT.json", receipt)
    # Also expose the exact inventory in the working tree for continuation.
    copy_file(stage / "RELEASE_INVENTORY.json", ROOT / "RELEASE_INVENTORY.json")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
