"""Shared compiler/cache support for actual generated-kernel validation.

All subprocesses use argument arrays, never a shell. This module does not install
software or download dependencies. Cache identities cover the compiler binary,
flags, every core source, and every public header.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compiler_config(compiler=None, sanitize=False):
    selected = compiler or os.environ.get("CXX") or ("cl.exe" if os.name == "nt" else "c++")
    executable = shutil.which(selected)
    if executable is None:
        raise RuntimeError(f"C++20 compiler not found: {selected}")
    resolved = Path(executable).resolve()
    msvc = resolved.name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}
    if msvc and sanitize:
        raise RuntimeError("Combined AddressSanitizer/UBSan generation tests require GCC or Clang")
    if msvc:
        flags = ["/nologo", "/std:c++20", "/EHsc", "/O2", "/DNDEBUG", "/W4", "/permissive-", "/Zc:__cplusplus"]
    else:
        flags = ["-std=c++20", "-O3", "-DNDEBUG", "-pthread", "-Wall", "-Wextra", "-Wpedantic"]
        if sanitize:
            flags = ["-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    version = subprocess.run([str(resolved), "/Bv" if msvc else "--version"], text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False).stdout
    return {"executable": str(resolved), "compiler_sha256": sha256(resolved),
            "version": version, "msvc": msvc, "flags": flags, "sanitize": sanitize}


def execute(argv, log_path, cwd=None, env=None):
    started = time.perf_counter()
    with Path(log_path).open("w", encoding="utf-8") as stream:
        result = subprocess.run([str(x) for x in argv], cwd=cwd, env=env,
                                stdout=stream, stderr=subprocess.STDOUT, check=False)
    wall_ms = (time.perf_counter() - started) * 1000
    if result.returncode:
        raise RuntimeError(f"Command failed with status {result.returncode}; see {log_path}")
    return wall_ms


def ensure_support(config, cache_parent=None):
    sources = sorted((ROOT / "src").glob("*.cpp"))
    sources = [path for path in sources if path.name != "main.cpp"]
    headers = sorted((ROOT / "include").rglob("*.hpp"))
    identities = {str(path.relative_to(ROOT)): sha256(path) for path in sources + headers}
    identity = {"compiler": config, "inputs": identities}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    cache = Path(cache_parent or ROOT / "build/codegen_cache") / key
    manifest = cache / "manifest.json"
    library = cache / ("suffice_support.lib" if config["msvc"] else "libsuffice_support.a")
    if manifest.exists() and library.exists():
        previous = json.loads(manifest.read_text(encoding="utf-8"))
        if previous.get("library_sha256") != sha256(library):
            raise RuntimeError("Generated-kernel support cache failed integrity check")
        return library, {**previous, "cache_hit": True, "this_invocation_build_ms": 0.0}
    cache.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    objects = []
    commands = []
    for source in sources:
        obj = cache / (source.stem + (".obj" if config["msvc"] else ".o"))
        if config["msvc"]:
            argv = [config["executable"], *config["flags"], "/I" + str(ROOT / "include"),
                    "/c", str(source), "/Fo" + str(obj)]
        else:
            argv = [config["executable"], *config["flags"], "-I", str(ROOT / "include"),
                    "-c", str(source), "-o", str(obj)]
        execute(argv, cache / (source.stem + ".log"), cwd=cache)
        objects.append(obj)
        commands.append(argv)
    archiver = shutil.which("lib.exe" if config["msvc"] else "ar")
    if archiver is None:
        raise RuntimeError("Static library archiver not found")
    argv = [archiver, "/nologo", "/OUT:" + str(library), *map(str, objects)] if config["msvc"] else [archiver, "rcs", str(library), *map(str, objects)]
    execute(argv, cache / "archive.log", cwd=cache)
    commands.append(argv)
    final_sources = sorted(path for path in (ROOT / "src").glob("*.cpp") if path.name != "main.cpp")
    final_headers = sorted((ROOT / "include").rglob("*.hpp"))
    final_identities = {str(path.relative_to(ROOT)): sha256(path) for path in final_sources + final_headers}
    if final_identities != identities:
        raise RuntimeError("Core inputs changed during support compilation; no cache manifest was committed. Rerun after sources settle.")
    elapsed = (time.perf_counter() - started) * 1000
    record = {"cache_key": key, "identity": identity, "commands": commands,
              "original_build_ms": elapsed, "library_sha256": sha256(library)}
    manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return library, {**record, "cache_hit": False, "this_invocation_build_ms": elapsed}


def compile_program(config, source, executable, library, log_path, include=None):
    source, executable = Path(source), Path(executable)
    if config["msvc"]:
        argv = [config["executable"], *config["flags"], "/I" + str(ROOT / "include")]
        if include:
            argv.append("/I" + str(include))
        argv += [str(source), str(library), "/Fe:" + str(executable),
                 "/Fo" + str(executable.with_suffix(".obj"))]
    else:
        argv = [config["executable"], *config["flags"], "-I", str(ROOT / "include")]
        if include:
            argv += ["-I", str(include)]
        argv += [str(source), str(library), "-o", str(executable)]
    elapsed = execute(argv, log_path, cwd=source.parent)
    return {"argv": argv, "wall_ms": elapsed, "binary_sha256": sha256(executable),
            "source_sha256": sha256(source)}
