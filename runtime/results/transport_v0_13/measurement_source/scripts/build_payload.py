#!/usr/bin/env python3
"""Build Suffice, its payload application, and the persistent catalog directly with a C++20 compiler.

No CMake, package manager, shell commands, or downloaded dependencies are used.
Run from a configured native compiler environment (a Developer Command Prompt
for MSVC). Zstandard headers and a linkable library must already be installed.
The output directory must be new. Content-addressed core objects are reused from
build/codegen_cache; all application/test objects belong to the output directory.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import sys
import time

import codegen_support as support
from codegen_support import compiler_config, ensure_support, execute, sha256


ROOT = Path(__file__).resolve().parent.parent
ASAN_OPTIONS = "detect_leaks=0:halt_on_error=1"
UBSAN_OPTIONS = "halt_on_error=1:print_stacktrace=1"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def input_paths(build_tests):
    paths = set((ROOT / "src").glob("*.cpp"))
    paths.update((ROOT / "include").rglob("*.hpp"))
    paths.update((ROOT / "apps/payload").glob("*.cpp"))
    paths.update((ROOT / "apps/payload").glob("*.hpp"))
    paths.update((ROOT / "apps/catalog").glob("*.cpp"))
    paths.update((ROOT / "apps/catalog").glob("*.hpp"))
    paths.update([Path(__file__).resolve(), ROOT / "scripts/codegen_support.py"])
    if build_tests:
        paths.update((ROOT / "tests").glob("*.cpp"))
        paths.update((ROOT / "tests").glob("*.hpp"))
    return sorted(paths)


def input_hashes(build_tests, external_files):
    return {
        "project": {str(path.relative_to(ROOT)): sha256(path) for path in input_paths(build_tests)},
        "explicit_external": {str(path): sha256(path) for path in sorted(external_files)},
    }


def changed_inputs(before, after):
    result = {}
    for section in before:
        initial, final = before[section], after[section]
        result[section] = {
            "added": sorted(final.keys() - initial.keys()),
            "removed": sorted(initial.keys() - final.keys()),
            "changed": sorted(key for key in initial.keys() & final.keys() if initial[key] != final[key]),
        }
    return result


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="new directory for binaries, objects, logs, and manifest")
    parser.add_argument("--compiler", help="compiler executable; default CXX, then cl.exe on Windows or c++ elsewhere")
    parser.add_argument("--sanitize", action="store_true", help="GCC/Clang AddressSanitizer + UBSan; leak detection disabled")
    parser.add_argument("--zstd-include", type=Path, help="directory containing zstd.h; otherwise use compiler include search")
    parser.add_argument("--zstd-library", type=Path, help="existing library path; default -lzstd for GCC/Clang; required .lib for MSVC")
    parser.add_argument("--no-tests", action="store_true", help="skip building and running test executables and CLI smoke checks")
    return parser.parse_args()


def main():
    args = parse_args()
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        raise SystemExit(f"Use a new --output directory; refusing existing path: {output}")
    objects_dir, logs_dir = output / "objects", output / "logs"
    objects_dir.mkdir()
    logs_dir.mkdir()
    manifest_path = output / "build_manifest.json"
    started = time.perf_counter()
    manifest = {
        "schema_version": 1,
        "version": "0.13.0",
        "status": "building",
        "phase": "configuration",
        "started_utc": utc_now(),
        "invocation": [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
        "root": str(ROOT),
        "output": str(output),
        "python": sys.version,
        "platform": platform.platform(),
        "tests_requested": not args.no_tests,
        "sanitize": args.sanitize,
        "test_environment_overrides": {"ASAN_OPTIONS": ASAN_OPTIONS, "UBSAN_OPTIONS": UBSAN_OPTIONS} if args.sanitize else {},
        "leak_detection": "disabled" if args.sanitize else "not_instrumented",
        "commands": [],
        "binaries": {},
        "objects": {},
        "tests": {},
        "input_stability": "not_checked",
    }
    config, library = None, None
    external_files = set()

    def save():
        temporary = manifest_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(manifest_path)

    def run(argv, log_path, cwd=None, env=None):
        """Audit the existing execute helper, including calls inside ensure_support."""
        command = {
            "phase": manifest["phase"],
            "argv": [str(value) for value in argv],
            "cwd": str(Path(cwd or ROOT).resolve()),
            "log": str(Path(log_path).resolve()),
            "started_utc": utc_now(),
            "status": "running",
        }
        manifest["commands"].append(command)
        save()
        tick = time.perf_counter()
        try:
            command["wall_ms"] = execute(argv, log_path, cwd=cwd, env=env)
            command.update(status="passed", returncode=0)
        except Exception as error:
            command.update(status="failed", error=str(error), wall_ms=(time.perf_counter() - tick) * 1000)
            code = re.search(r"Command failed with status (-?\d+)", str(error))
            if code:
                command["returncode"] = int(code.group(1))
            raise
        finally:
            command["finished_utc"] = utc_now()
            if Path(log_path).is_file():
                command["log_sha256"] = sha256(log_path)
            save()
        return command["wall_ms"]

    save()
    try:
        config = compiler_config(args.compiler, args.sanitize)
        manifest["compiler"] = config
        manifest["compiler_probe"] = {
            "argv": [config["executable"], "/Bv" if config["msvc"] else "--version"],
            "output": config["version"],
        }
        includes = [ROOT / "include"]
        if args.zstd_include:
            include = args.zstd_include.resolve()
            if not include.is_dir() or not (include / "zstd.h").is_file():
                raise RuntimeError(f"--zstd-include must contain zstd.h: {include}")
            includes.append(include)
            external_files.update(path for path in include.glob("zstd*.h") if path.is_file())
        if args.zstd_library:
            zstd = args.zstd_library.resolve()
            if not zstd.is_file():
                raise RuntimeError(f"--zstd-library is not an existing file: {zstd}")
            if config["msvc"] and zstd.suffix.lower() != ".lib":
                raise RuntimeError("MSVC requires an explicit .lib path for --zstd-library")
            zstd_link = [str(zstd)]
            external_files.add(zstd)
        else:
            if config["msvc"]:
                raise RuntimeError("MSVC requires --zstd-library PATH_TO_ZSTD.lib")
            zstd_link = ["-lzstd"]
        # macOS provides dlopen in libSystem; Windows uses LoadLibrary directly.
        dynamic_link = [] if os.name == "nt" or sys.platform == "darwin" else ["-ldl"]
        manifest["dependencies"] = {
            "zstd_include_argument": str(args.zstd_include.resolve()) if args.zstd_include else None,
            "zstd_link_arguments": zstd_link,
            "dynamic_loader_link_arguments": dynamic_link,
            "implicit_system_dependency_hashes": "not_collected; compiler default headers/libraries are environment dependencies",
        }
        manifest["inputs_before"] = input_hashes(not args.no_tests, external_files)
        save()

        print("Building or reusing the content-addressed core support library", flush=True)
        manifest["phase"] = "core_support"
        # ensure_support calls its module's execute. Route those calls through
        # the audit wrapper while retaining the original subprocess helper.
        original_execute = support.execute
        support.execute = run
        try:
            library, support_record = ensure_support(config)
        finally:
            support.execute = original_execute
        manifest["support"] = {"library": str(library), **support_record}
        save()

        object_suffix = ".obj" if config["msvc"] else ".o"
        executable_suffix = ".exe" if config["msvc"] or os.name == "nt" else ""

        def compile_object(relative, assertions=False):
            source = ROOT / relative
            stem = "__".join(source.relative_to(ROOT).with_suffix("").parts)
            obj = objects_dir / (stem + object_suffix)
            flags = [*config["flags"]]
            if assertions:
                # Place UNDEBUG after the Release configuration's DNDEBUG.
                flags.append("/UNDEBUG" if config["msvc"] else "-UNDEBUG")
            if config["msvc"]:
                argv = [config["executable"], *flags, *["/I" + str(path) for path in includes],
                        "/c", str(source), "/Fo" + str(obj)]
            else:
                include_flags = [argument for path in includes for argument in ("-I", str(path))]
                argv = [config["executable"], *flags, *include_flags, "-c", str(source), "-o", str(obj)]
            manifest["phase"] = "compile:" + relative
            print("Compiling " + relative, flush=True)
            elapsed = run(argv, logs_dir / (stem + ".compile.log"), cwd=output)
            manifest["objects"][relative] = {
                "path": str(obj), "sha256": sha256(obj), "source_sha256": sha256(source),
                "assertions_enabled": assertions, "wall_ms": elapsed,
            }
            save()
            return obj

        def link(name, objects, extra=()):
            binary = output / (name + executable_suffix)
            if config["msvc"]:
                argv = [config["executable"], *config["flags"], *map(str, objects), str(library),
                        *extra, "/Fe:" + str(binary)]
            else:
                argv = [config["executable"], *config["flags"], *map(str, objects), str(library),
                        *extra, "-o", str(binary)]
            manifest["phase"] = "link:" + name
            print("Linking " + name, flush=True)
            elapsed = run(argv, logs_dir / (name + ".link.log"), cwd=output)
            manifest["binaries"][name] = {"path": str(binary), "sha256": sha256(binary), "link_ms": elapsed}
            save()
            return binary

        core_cli = link("suffice", [compile_object("src/main.cpp")])
        payload_object = compile_object("apps/payload/payload.cpp")
        payload_cli = link("suffice_payload", [compile_object("apps/payload/main.cpp"), payload_object], zstd_link)
        benchmark_objects = [compile_object("apps/payload/benchmark.cpp"), compile_object("apps/payload/sqlite_gate.cpp"), payload_object]
        link("suffice_payload_bench", benchmark_objects, [*zstd_link, *dynamic_link])

        catalog_object = compile_object("apps/catalog/catalog.cpp")
        generation_object = compile_object("apps/catalog/generation.cpp")
        catalog_cli = link("suffice_catalog", [compile_object("apps/catalog/main.cpp"), catalog_object, generation_object, payload_object], zstd_link)

        if not args.no_tests:
            catalog_test_objects = [compile_object(path, assertions=True)
                                    for path in ("tests/catalog_test_main.cpp", "tests/test_catalog.cpp")]
            catalog_tests = link("suffice_catalog_tests", [*catalog_test_objects, catalog_object, payload_object], zstd_link)
            digest_tests = link("suffice_digest_tests", [compile_object("tests/catalog_digest_main.cpp", assertions=True)])
            wire_tests = link("suffice_wire_tests", [compile_object("tests/catalog_wire_main.cpp", assertions=True)])
            payload_test_objects = [compile_object(path, assertions=True)
                                    for path in ("tests/payload_test_main.cpp", "tests/test_payload.cpp")]
            payload_tests = link("suffice_payload_tests", [*payload_test_objects, payload_object], zstd_link)
            core_test_sources = [ROOT / "tests/main.cpp", *sorted(path for path in (ROOT / "tests").glob("test_*.cpp")
                                                                if path.name not in {"test_payload.cpp", "test_catalog.cpp"})]
            core_test_objects = [compile_object(str(path.relative_to(ROOT)), assertions=True) for path in core_test_sources]
            core_tests = link("suffice_tests", core_test_objects)
            environment = os.environ.copy()
            environment.update(manifest["test_environment_overrides"])
            checks = [
                ("catalog_correctness", [catalog_tests]),
                ("digest_correctness", [digest_tests]),
                ("wire_correctness", [wire_tests]),
                ("payload_correctness", [payload_tests]),
                ("core_correctness", [core_tests]),
                ("core_cli_version", [core_cli, "--version"]),
                ("payload_cli_version", [payload_cli, "--version"]),
                ("payload_cli_help", [payload_cli, "--help"]),
                ("catalog_cli_version", [catalog_cli, "--version"]),
                ("catalog_cli_help", [catalog_cli, "--help"]),
            ]
            for name, argv in checks:
                manifest["phase"] = "test:" + name
                print("Running " + name, flush=True)
                log = logs_dir / (name + ".test.log")
                manifest["tests"][name] = {"status": "running", "log": str(log)}
                elapsed = run(argv, log, cwd=output, env=environment)
                manifest["tests"][name].update(status="passed", wall_ms=elapsed, log_sha256=sha256(log))
                save()
        manifest["phase"] = "final_identity_check"
        manifest["status"] = "passed"
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        print(f"Build failed: {error}", file=sys.stderr, flush=True)
    finally:
        try:
            if "inputs_before" in manifest:
                final = input_hashes(not args.no_tests, external_files)
                manifest["inputs_after"] = final
                stable = final == manifest["inputs_before"]
                manifest["input_stability"] = "unchanged" if stable else "changed"
                manifest["input_changes"] = changed_inputs(manifest["inputs_before"], final)
                if not stable:
                    manifest["status"] = "failed"
                    manifest["input_stability_error"] = "Build inputs changed during execution; rerun from settled sources into a new output directory."
            if config:
                manifest["compiler_sha256_after"] = sha256(config["executable"])
                if manifest["compiler_sha256_after"] != config["compiler_sha256"]:
                    manifest["status"] = "failed"
                    manifest["compiler_stability_error"] = "Compiler executable changed during execution."
            if library:
                manifest["support_library_sha256_after"] = sha256(library)
                if manifest["support_library_sha256_after"] != manifest["support"]["library_sha256"]:
                    manifest["status"] = "failed"
                    manifest["support_stability_error"] = "Core support archive changed during execution."
            for name, result in manifest["tests"].items():
                if result["status"] == "running":
                    result["status"] = "failed"
                    log = Path(result["log"])
                    if log.is_file():
                        result["log_sha256"] = sha256(log)
        except Exception as error:
            manifest["status"] = "failed"
            manifest["identity_check_error"] = str(error)
        manifest["finished_utc"] = utc_now()
        manifest["total_wall_ms"] = (time.perf_counter() - started) * 1000
        if manifest["status"] == "passed":
            manifest["phase"] = "complete"
        save()
    print(f"{manifest['status'].upper()}: {manifest_path}", flush=True)
    return 0 if manifest["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
