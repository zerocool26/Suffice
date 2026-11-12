#!/usr/bin/env python3
"""Package current source/evidence without recursive prior build duplication."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import stat
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    state = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            state.update(block)
    return state.hexdigest()


def selected(relative, compact):
    parts = relative.parts
    if any(x in {"__pycache__", ".git", ".pytest_cache"} for x in parts):
        return False
    if relative.suffix in {".pyc", ".o", ".obj", ".a", ".lib", ".pdb"}:
        return False
    if parts[0] == "build":
        if len(parts) >= 3 and parts[1] == "codegen_cache":
            return not compact and relative.name.endswith((".log", "manifest.json"))
        if len(parts) < 3 or parts[1] not in {"catalog_release", "catalog_sanitize"}:
            return False
        if relative.name == "build_manifest.json":
            return True
        if len(parts) >= 4 and parts[2] == "logs":
            return not compact or relative.name.endswith(".test.log")
        if parts[1] == "catalog_release" and len(parts) == 3 and not compact:
            manifest = json.loads((ROOT / "build/catalog_release/build_manifest.json").read_text())
            return relative.name in manifest["binaries"]
        return False
    if parts[0] == "baseline":
        return not compact or relative.name == "PARENT_RELEASE.json"
    if parts[0] != "results" or not compact:
        return True
    if parts[1] in {"presentation_v0_11", "research_review_v0_11", "runtime_identity_v0_11",
                    "catalog_sessions_audit_v0_11", "artifact_ingest_v0_11"}:
        return True
    if parts[1] == "catalog_sessions_v0_11" and len(parts) == 3:
        return relative.name in {"SUMMARY.json", "TABLES.md", "PORTABLE_AUDIT_CURRENT.json"}
    if parts[1] == "catalog_integration_v0_11" and len(parts) == 4:
        return parts[2] in {"release_closed", "sanitize_closed"} and relative.name in {"RESULT.json", "EXECUTION_RECEIPT.json"}
    return False


def archive(destination, compact):
    if destination.exists():
        raise FileExistsError(destination)
    prefix = "Suffice_0_11_0_Source" if compact else "Suffice_0_11_0"
    included, omitted_links = [], []
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if path.is_symlink():
            if not compact:
                omitted_links.append({"path": relative.as_posix(), "target": os.readlink(path),
                                      "reason": "corruption-test symlink represented as data, not installed by extraction"})
            continue
        if path.is_file() and selected(relative, compact):
            included.append((path, relative))
    records = [{"path": relative.as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
               for path, relative in included]
    manifest = {"version": "0.11.0", "distribution": "source" if compact else "evidence",
                "files": records, "omitted_symlink_fixtures": omitted_links,
                "scope": "Exact retained files; generated objects/core caches and sanitizer executables omitted; compact source also omits raw inputs/frozen sources/Release binaries/predecessor ZIP."}
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path, relative in included:
            bundle.write(path, prefix + "/" + relative.as_posix())
        bundle.writestr(prefix + "/PACKAGE_MANIFEST.json", json.dumps(manifest, indent=2) + "\n")
    with zipfile.ZipFile(destination) as bundle:
        bad = bundle.testzip()
        if bad:
            raise RuntimeError("ZIP CRC failure: " + bad)
        if len(bundle.namelist()) != len(records) + 1:
            raise RuntimeError("archive membership differs from inventory")
        for entry in records:
            body = bundle.read(prefix + "/" + entry["path"])
            if len(body) != entry["bytes"] or hashlib.sha256(body).hexdigest() != entry["sha256"]:
                raise RuntimeError("archive file differs from recorded source: " + entry["path"])
    return {"file_name": destination.name, "bytes": destination.stat().st_size,
            "sha256": digest(destination), "member_count": len(records) + 1,
            "omitted_symlink_count": len(omitted_links), "crc_and_file_hashes": "passed"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path, required=True, help="fresh directory outside the project")
    args = ap.parse_args()
    output = args.output.resolve()
    if output == ROOT or output.is_relative_to(ROOT):
        ap.error("output must be outside project to prevent recursive inclusion")
    output.mkdir(parents=True, exist_ok=False)
    receipt = {"version": "0.11.0", "archives": []}
    receipt["archives"].append(archive(output / "Suffice_0_11_0_Source.zip", True))
    receipt["archives"].append(archive(output / "Suffice_0_11_0.zip", False))
    report = (ROOT / "REPORT.md").read_text()
    report = report.replace("(results/presentation_v0_11/catalog_sessions.png)", "(Suffice_0_11_0_Results.png)")
    report = report.replace("## 1. Completed outcome", "The companion PNG and extracted evidence archive supply the chart and relative evidence paths below. The narrative and result tables are complete in this file.\n\n## 1. Completed outcome", 1)
    (output / "Suffice_0_11_0_Report.md").write_text(report)
    shutil.copyfile(ROOT / "results/presentation_v0_11/catalog_sessions.png", output / "Suffice_0_11_0_Results.png")
    receipt["companions"] = [{"file_name": path.name, "bytes": path.stat().st_size, "sha256": digest(path)}
                             for path in (output / "Suffice_0_11_0_Report.md", output / "Suffice_0_11_0_Results.png")]
    (output / "Suffice_0_11_0_Package_Receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
