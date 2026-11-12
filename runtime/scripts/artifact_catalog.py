#!/usr/bin/env python3
"""Capture a bounded project tree and publish an immutable Suffice catalog generation.

The producer owns only its catalog directory. Existing generations and source
files are never pruned or modified. A capture is a set of individually checked
file reads plus a reconciled inventory, not a filesystem-wide transaction.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
import uuid

from payload_store import write_store


SCHEMA = 1
CHANNELS = 4
COMPONENTS = ("features.i32", "features.sfp", "payload.spp", "rows.tsv")
DEFAULT_EXCLUDES = frozenset({
    ".git", ".hg", ".svn", "node_modules", "target", "build", "dist", "out",
    ".venv", "venv", "__pycache__", ".mypy_cache", ".pytest_cache", ".ruff_cache",
    ".next", ".nuxt", ".turbo", ".cache", "CMakeFiles", "bin", "obj",
})
EXTENSION_CLASSES = {
    **{s: 1 for s in (".rs",)},
    **{s: 2 for s in (".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx", ".cu", ".cuh")},
    **{s: 3 for s in (".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs")},
    **{s: 4 for s in (".py", ".pyi")},
    **{s: 5 for s in (".json", ".jsonc", ".toml", ".yaml", ".yml", ".ini", ".cfg", ".xml")},
    **{s: 6 for s in (".md", ".markdown", ".rst")},
    **{s: 7 for s in (".glsl", ".wgsl", ".vert", ".frag", ".hlsl", ".metal")},
    **{s: 8 for s in (".log", ".txt")},
}
GENERATION_RE = re.compile(r"[0-9a-f]{32}\Z")
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")
DECIMAL_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")


class CaptureError(RuntimeError):
    """Capture/publication error; CURRENT is authoritative after interruption."""


@dataclass(frozen=True)
class CaptureLimits:
    max_files: int = 100_000
    max_file_bytes: int = 1 << 20
    max_total_decoded_bytes: int = 128 << 20
    max_generation_bytes: int = 512 << 20
    max_catalog_bytes: int = 1 << 30
    max_generations: int = 16
    max_rows_metadata_bytes: int = 16 << 20
    max_path_bytes: int = 4096
    max_directories: int = 100_000
    max_entries: int = 1_000_000
    max_depth: int = 64

    def validate(self):
        for name, value in asdict(self).items():
            if type(value) is not int or value < 0:
                raise ValueError(f"{name} must be a nonnegative integer")
        if self.max_files > 1 << 20:
            raise ValueError("max_files exceeds the payload format admission limit")
        if self.max_file_bytes > 0x7FFFFFFF:
            raise ValueError("max_file_bytes exceeds signed feature representation")
        if self.max_total_decoded_bytes > 4 << 30:
            raise ValueError("max_total_decoded_bytes exceeds the default native limit")
        if self.max_path_bytes > 4096:
            raise ValueError("max_path_bytes exceeds the catalog schema limit")
        if self.max_generation_bytes > 512 << 20:
            raise ValueError("max_generation_bytes exceeds this producer's bounded format policy")
        if self.max_rows_metadata_bytes > 16 << 20:
            raise ValueError("max_rows_metadata_bytes exceeds the catalog metadata policy")
        if self.max_depth > 64:
            raise ValueError("max_depth exceeds the catalog capture policy")


def _is_reparse(info):
    return bool(getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def _stamp(info):
    # ctime is change time on POSIX; creation time semantics differ on Windows.
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _check_plain(info, directory=False):
    if stat.S_ISLNK(info.st_mode) or _is_reparse(info):
        raise CaptureError("symbolic links and reparse points are not admitted")
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise CaptureError("expected an ordinary directory" if directory else "only ordinary files are admitted")


def _absolute_plain(path, *, allow_missing=False):
    result = Path(os.path.abspath(os.path.expanduser(os.fspath(path))))
    cursor = Path(result.anchor)
    for part in result.parts[1:]:
        cursor /= part
        try:
            info = cursor.lstat()
        except FileNotFoundError:
            if allow_missing:
                continue
            raise CaptureError(f"path does not exist: {cursor}")
        if stat.S_ISLNK(info.st_mode) or _is_reparse(info):
            raise CaptureError(f"symbolic/reparse path component: {cursor}")
    return result


def validate_relative_path(text, max_path_bytes=4096):
    if not isinstance(text, str):
        raise CaptureError("relative path must be text")
    try:
        encoded = text.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise CaptureError("path is not representable as strict UTF-8") from error
    if (not encoded or len(encoded) > max_path_bytes or text.startswith("/")
            or any(c in text for c in ("\\", "\x00", ":"))
            or any(part in ("", ".", "..") for part in text.split("/"))):
        raise CaptureError(f"noncanonical or over-budget relative path: {text!r}")
    return encoded


def extension_class(relative):
    name = relative.rsplit("/", 1)[-1]
    dot = name.rfind(".")
    if dot <= 0:
        return 0
    suffix = "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in name[dot:])
    return EXTENSION_CLASSES.get(suffix, 0)


def derive_features(relative, data):
    try:
        data.decode("utf-8", errors="strict")
        text = int(b"\0" not in data)
    except UnicodeDecodeError:
        text = 0
    return len(data), extension_class(relative), data.count(b"\n"), text


@contextmanager
def _posix_directory(path):
    # Resolve every component through already opened directories. O_NOFOLLOW
    # blocks both an existing symlink and a swap to a symlink before each open.
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0)
    handles = []
    try:
        handle = os.open(path.anchor, flags)
        handles.append(handle)
        for part in path.parts[1:]:
            handle = os.open(part, flags, dir_fd=handle)
            handles.append(handle)
        yield handle
    finally:
        for handle in reversed(handles):
            os.close(handle)


@contextmanager
def _windows_handles(path, *, file=False):
    """Pin each ancestor without delete sharing and reject reparse objects.

    This branch is implementation-reviewed but has no native Windows execution
    evidence in the 0.11 Linux validation environment.
    """
    import ctypes
    from ctypes import wintypes
    import msvcrt

    class FileInfo(ctypes.Structure):
        _fields_ = [("attributes", wintypes.DWORD), ("creation", wintypes.FILETIME),
                    ("access", wintypes.FILETIME), ("write", wintypes.FILETIME),
                    ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                    ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                    ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    create.restype = wintypes.HANDLE
    inspect = kernel.GetFileInformationByHandle
    inspect.argtypes = (wintypes.HANDLE, ctypes.POINTER(FileInfo))
    inspect.restype = wintypes.BOOL
    close = kernel.CloseHandle
    close.argtypes = (wintypes.HANDLE,)
    close.restype = wintypes.BOOL
    invalid = ctypes.c_void_p(-1).value
    handles = []
    fd = None
    try:
        cursor = Path(path.anchor)
        pieces = [cursor]
        for part in path.parts[1:]:
            cursor /= part
            pieces.append(cursor)
        for index, item in enumerate(pieces):
            is_file = file and index == len(pieces) - 1
            access = 0x80000000 if is_file else 0x80  # GENERIC_READ / READ_ATTRIBUTES
            sharing = 1 if is_file else 3  # deny writes for the file; deny delete for all
            flags = 0x00200000 | (0x08000000 if is_file else 0x02000000)
            native_path = str(item)
            if not native_path.startswith("\\\\?\\"):
                native_path = "\\\\?\\UNC\\" + native_path[2:] if native_path.startswith("\\\\") else "\\\\?\\" + native_path
            handle = create(native_path, access, sharing, None, 3, flags, None)
            if handle == invalid:
                raise ctypes.WinError(ctypes.get_last_error())
            handles.append(handle)
            info = FileInfo()
            if not inspect(handle, ctypes.byref(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            if info.attributes & 0x400:
                raise CaptureError(f"reparse point is not admitted: {item}")
            is_directory = bool(info.attributes & 0x10)
            if is_directory == is_file:
                raise CaptureError("ordinary file/directory kind changed during capture")
        if file:
            fd = msvcrt.open_osfhandle(handles[-1], os.O_RDONLY | os.O_BINARY)
            handles.pop()  # descriptor now owns this handle
        yield fd
    finally:
        if fd is not None:
            os.close(fd)
        for handle in reversed(handles):
            close(handle)


@contextmanager
def _directory_entries(path):
    if os.name == "nt":
        with _windows_handles(path):
            before = path.stat(follow_symlinks=False)
            _check_plain(before, directory=True)
            with os.scandir(path) as entries:
                yield before, entries
    else:
        with _posix_directory(path) as handle:
            before = os.fstat(handle)
            with os.scandir(handle) as entries:
                yield before, entries


@contextmanager
def _regular_file(path):
    if os.name == "nt":
        with _windows_handles(path, file=True) as handle:
            _check_plain(os.fstat(handle))
            yield handle
    else:
        with _posix_directory(path.parent) as directory:
            # A raced replacement by a FIFO must not block before fstat can
            # reject its kind. O_NONBLOCK does not change regular-file reads.
            handle = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
                             | getattr(os, "O_CLOEXEC", 0), dir_fd=directory)
            try:
                _check_plain(os.fstat(handle))
                yield handle
            finally:
                os.close(handle)


def _read_stable(path, max_bytes, expected=None):
    with _regular_file(path) as handle:
        before = os.fstat(handle)
        if expected is not None and _stamp(before) != expected:
            raise CaptureError(f"file changed after enumeration: {path}")
        if before.st_size > max_bytes:
            raise CaptureError(f"file exceeds byte budget: {path}")
        parts, remaining = [], max_bytes + 1
        while remaining:
            part = os.read(handle, min(1 << 20, remaining))
            if not part:
                break
            parts.append(part)
            remaining -= len(part)
        data = b"".join(parts)
        after = os.fstat(handle)
        if len(data) > max_bytes or len(data) != before.st_size or _stamp(before) != _stamp(after):
            raise CaptureError(f"file changed or exceeded its limit during capture: {path}")
    return data


def _entry_metadata(entry, path, *, windows=os.name == "nt"):
    # Windows DirEntry.stat() omits st_dev/st_ino (returns zero). A full stat
    # is needed to compare enumeration identity with the later opened handle.
    # The caller still holds the ancestor directory handles while doing this.
    return path.stat(follow_symlinks=False) if windows else entry.stat(follow_symlinks=False)


def _inventory(root, catalog, limits, excludes):
    files, directories = {}, {}
    skipped = 0
    entries_seen = 0
    declared_bytes = 0
    catalog_info = catalog.stat(follow_symlinks=False)
    _check_plain(catalog_info, directory=True)
    if catalog_info.st_ino == 0:
        raise CaptureError("filesystem does not expose the catalog directory identity")
    catalog_identity = (catalog_info.st_dev, catalog_info.st_ino)
    stack = [(root, "", 0)]
    while stack:
        directory, prefix, depth = stack.pop()
        if depth > limits.max_depth:
            raise CaptureError("directory depth budget exceeded")
        if len(directories) >= limits.max_directories:
            raise CaptureError("directory count budget exceeded")
        with _directory_entries(directory) as (before, entries):
            directories[prefix] = _stamp(before)
            for entry in entries:
                entries_seen += 1
                if entries_seen > limits.max_entries:
                    raise CaptureError("directory entry budget exceeded")
                child = directory / entry.name
                if entry.name in excludes:
                    skipped += 1
                    continue
                relative = prefix + entry.name
                info = _entry_metadata(entry, child)
                if stat.S_ISLNK(info.st_mode) or _is_reparse(info):
                    raise CaptureError(f"symbolic/reparse entry is not admitted: {relative!r}")
                # WindowsPath equality folds case even in case-sensitive NTFS
                # directories. Exclude the actual store, not a distinct sibling
                # whose spelling happens to differ only in case.
                if stat.S_ISDIR(info.st_mode) and (info.st_dev, info.st_ino) == catalog_identity:
                    skipped += 1
                    continue
                validate_relative_path(relative, limits.max_path_bytes)
                if stat.S_ISDIR(info.st_mode):
                    stack.append((child, relative + "/", depth + 1))
                else:
                    _check_plain(info)
                    if len(files) >= limits.max_files:
                        raise CaptureError("file count budget exceeded")
                    if info.st_size > limits.max_file_bytes:
                        raise CaptureError(f"file byte budget exceeded: {relative!r}")
                    declared_bytes += info.st_size
                    if declared_bytes > limits.max_total_decoded_bytes:
                        raise CaptureError("total decoded-byte budget exceeded")
                    files[relative] = _stamp(info)
            after = directory.stat(follow_symlinks=False)
            _check_plain(after, directory=True)
            if _stamp(after) != _stamp(before):
                raise CaptureError(f"directory changed while enumerating: {directory}")
    return {"files": files, "directories": directories, "skipped": skipped,
            "entries_seen": entries_seen, "declared_bytes": declared_bytes}


def _same_inventory(first, second):
    return first["files"] == second["files"] and first["directories"] == second["directories"]


def _write_exclusive(path, data):
    with open(path, "xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def _flush_directory(path):
    if os.name == "nt":
        return False
    with _posix_directory(path) as handle:
        try:
            os.fsync(handle)
        except OSError as error:
            import errno
            if error.errno in (errno.EINVAL, errno.ENOTSUP, errno.EBADF):
                return False
            raise
    return True


def read_current(catalog):
    path = Path(catalog) / "CURRENT"
    try:
        data = _read_stable(path, 33)
    except FileNotFoundError:
        return None
    if len(data) != 33 or data[-1:] != b"\n":
        raise CaptureError("CURRENT must be a 32-character generation followed by LF")
    try:
        generation = data[:-1].decode("ascii")
    except UnicodeError as error:
        raise CaptureError("CURRENT is not ASCII") from error
    if not GENERATION_RE.fullmatch(generation):
        raise CaptureError("CURRENT generation is not canonical lowercase hexadecimal")
    return generation


@contextmanager
def _publisher_lock(catalog):
    path = catalog / ".publish.lock"
    token = json.dumps({"pid": os.getpid(), "nonce": uuid.uuid4().hex,
                        "created_utc": datetime.now(timezone.utc).isoformat()}, sort_keys=True).encode() + b"\n"
    try:
        _write_exclusive(path, token)
    except FileExistsError as error:
        raise CaptureError("catalog publisher lock exists; inspect the owner before removing a stale lock") from error
    try:
        yield
    finally:
        # Never unlink a lock replaced or altered by another actor.
        try:
            if _read_stable(path, 4096) == token:
                path.unlink()
        except (OSError, CaptureError):
            pass


def _decimal(text):
    if not DECIMAL_RE.fullmatch(text):
        raise CaptureError("noncanonical decimal metadata")
    return int(text)


def _prior_generation(catalog, generation, limits):
    directory = catalog / "generations" / generation
    manifest_data = _read_stable(directory / "manifest.txt", 16 << 10)
    try:
        manifest = manifest_data.decode("ascii")
    except UnicodeError as error:
        raise CaptureError("prior manifest is not ASCII") from error
    lines = manifest.splitlines()
    if (not manifest.endswith("\n") or "\r" in manifest or len(lines) != 9
            or lines[0] != "SUFCAT01" or lines[1] != f"generation\t{generation}"
            or lines[3] != "channels\t4" or lines[4] != "schema\t1"):
        raise CaptureError("prior generation manifest schema differs")
    row_line = lines[2].split("\t")
    if len(row_line) != 2 or row_line[0] != "rows":
        raise CaptureError("prior manifest row count differs")
    count = _decimal(row_line[1])
    if count > limits.max_files:
        raise CaptureError("prior generation exceeds current file count budget")
    data = {}
    total = len(manifest_data)
    for line, name in zip(lines[5:], COMPONENTS):
        fields = line.split("\t")
        if len(fields) != 4 or fields[:2] != ["component", name] or not SHA_RE.fullmatch(fields[3]):
            raise CaptureError("prior component description differs")
        size = _decimal(fields[2])
        total += size
        if total > limits.max_generation_bytes:
            raise CaptureError("prior generation exceeds current byte budget")
        cap = limits.max_rows_metadata_bytes if name == "rows.tsv" else limits.max_generation_bytes
        component = _read_stable(directory / name, min(size, cap))
        if len(component) != size or hashlib.sha256(component).hexdigest() != fields[3]:
            raise CaptureError(f"prior component integrity failed: {name}")
        if name in ("rows.tsv", "features.i32"):
            data[name] = component
    rows = {}
    metadata = data["rows.tsv"]
    if metadata and not metadata.endswith(b"\n"):
        raise CaptureError("prior rows metadata lacks final LF")
    previous_path = None
    for index, line in enumerate(metadata.splitlines()):
        fields = line.split(b"\t")
        if len(fields) != 4 or fields[0] != str(index).encode("ascii"):
            raise CaptureError("prior row metadata differs")
        try:
            hex_path = fields[1].decode("ascii")
            path_bytes = bytes.fromhex(hex_path)
            if path_bytes.hex() != hex_path:
                raise ValueError("noncanonical hex")
            relative = path_bytes.decode("utf-8", errors="strict")
            validate_relative_path(relative, limits.max_path_bytes)
            size = _decimal(fields[2].decode("ascii"))
            digest = fields[3].decode("ascii")
        except (UnicodeError, ValueError) as error:
            raise CaptureError("prior row encoding differs") from error
        if not SHA_RE.fullmatch(digest) or size > limits.max_file_bytes:
            raise CaptureError("prior row exceeds schema/byte limit")
        if previous_path is not None and path_bytes <= previous_path:
            raise CaptureError("prior paths are not strictly sorted")
        previous_path = path_bytes
        rows[relative] = (size, digest)
    if len(rows) != count or len(data["features.i32"]) != count * CHANNELS * 4:
        raise CaptureError("prior row/raw dimensions differ")
    return {"rows": rows, "metadata": metadata, "raw": data["features.i32"]}


def _catalog_disk_usage(catalog, limits):
    """Charge all regular catalog files, including abandoned staging data.

    The cooperating writer lock serializes this accounting with this publisher.
    Allocated filesystem blocks, directory entries and external writers are
    outside the logical file-length budget.
    """
    total = count = 0
    generation_count = 0
    stack = [catalog]
    while stack:
        directory = stack.pop()
        with _directory_entries(directory) as (_, entries):
            for entry in entries:
                count += 1
                if count > limits.max_entries:
                    raise CaptureError("catalog inventory entry budget exceeded")
                info = _entry_metadata(entry, directory / entry.name)
                if stat.S_ISLNK(info.st_mode) or _is_reparse(info):
                    raise CaptureError("catalog contains a symbolic link or reparse point")
                if stat.S_ISDIR(info.st_mode):
                    stack.append(directory / entry.name)
                    if directory == catalog / "generations" and GENERATION_RE.fullmatch(entry.name):
                        generation_count += 1
                else:
                    _check_plain(info)
                    total += info.st_size
    return {"file_bytes": total, "generations": generation_count}


def capture(root, catalog, packer, *, expected_current=None, limits=None, excludes=DEFAULT_EXCLUDES):
    """Publish or reuse one generation; expected_current='none' asserts no CURRENT.

    The source must be quiescent for a coherent project-generation meaning.
    Logical budgets do not bound RSS, kernel caches, or all temporary allocations.
    """
    limits = limits or CaptureLimits()
    limits.validate()
    started = time.perf_counter()
    root = _absolute_plain(root)
    _check_plain(root.lstat(), directory=True)
    catalog = _absolute_plain(catalog, allow_missing=True)
    if root == catalog or root.is_relative_to(catalog):
        raise ValueError("catalog must not be the source root or its ancestor")
    excludes = frozenset(excludes)
    if len(excludes) > 256:
        raise ValueError("at most 256 exclusion names are supported")
    for name in excludes:
        if not isinstance(name, str) or not name or name in (".", "..") or any(c in name for c in ("/", "\\", "\0", ":")):
            raise ValueError("exclusions must be individual nonempty path components")
        if len(name.encode("utf-8")) > 256:
            raise ValueError("exclusion name exceeds 256 UTF-8 bytes")
    if expected_current is not None and expected_current != "none" and not GENERATION_RE.fullmatch(expected_current):
        raise ValueError("expected_current must be 'none' or a canonical generation")
    packer = _absolute_plain(packer)
    _check_plain(packer.lstat())
    catalog.mkdir(parents=True, exist_ok=True)
    _absolute_plain(catalog)
    _check_plain(catalog.lstat(), directory=True)
    generations = catalog / "generations"
    generations.mkdir(exist_ok=True)
    _absolute_plain(generations)
    _check_plain(generations.lstat(), directory=True)
    stage = None
    final_directory = None
    pointer_temp = None
    committed = False
    with _publisher_lock(catalog):
        current = read_current(catalog)
        if expected_current is not None and current != (None if expected_current == "none" else expected_current):
            raise CaptureError("expected CURRENT does not match; stale publication rejected")
        prior = _prior_generation(catalog, current, limits) if current else None
        before = _inventory(root, catalog, limits, excludes)
        captured = []
        raw = bytearray()
        metadata = bytearray()
        decoded_bytes = 0
        unchanged = 0
        for index, relative in enumerate(sorted(before["files"], key=lambda value: value.encode("utf-8"))):
            data = _read_stable(root / relative, limits.max_file_bytes, before["files"][relative])
            decoded_bytes += len(data)
            if decoded_bytes > limits.max_total_decoded_bytes:
                raise CaptureError("captured decoded-byte budget exceeded")
            digest = hashlib.sha256(data).hexdigest()
            features = derive_features(relative, data)
            raw.extend(struct.pack("<4i", *features))
            row = f"{index}\t{relative.encode('utf-8').hex()}\t{len(data)}\t{digest}\n".encode("ascii")
            if len(metadata) + len(row) > limits.max_rows_metadata_bytes:
                raise CaptureError("row metadata byte budget exceeded")
            metadata.extend(row)
            captured.append(data)
            if prior and prior["rows"].get(relative) == (len(data), digest):
                unchanged += 1
        after = _inventory(root, catalog, limits, excludes)
        if not _same_inventory(before, after):
            raise CaptureError("source inventory changed while capturing; previous generation retained")
        source_digest = hashlib.sha256(b"SUFCAT-SCHEMA-1\0" + metadata).hexdigest()
        current_paths = set(before["files"])
        prior_paths = set(prior["rows"]) if prior else set()
        result = {
            "schema_version": 1, "version": "0.13.0", "source_root": str(root),
            "catalog_root": str(catalog), "generation": current, "previous_generation": current,
            "rows": len(captured), "decoded_payload_bytes": decoded_bytes,
            "source_digest": source_digest, "unchanged_files": unchanged,
            "added_files": len(current_paths - prior_paths),
            "deleted_files": len(prior_paths - current_paths),
            "modified_files": len(current_paths & prior_paths) - unchanged,
            "skipped_entries": before["skipped"], "entries_seen": before["entries_seen"],
            "capture_semantics": "individually_stable_reads_and_reconciled_inventory; quiescent_source_required_for_coherent_project_snapshot",
            "file_bytes_rehashed": decoded_bytes, "limits": asdict(limits),
            "exclusions": sorted(excludes),
        }
        disk = _catalog_disk_usage(catalog, limits)
        result.update(catalog_retained_bytes_before=disk["file_bytes"],
                      retained_generations_before=disk["generations"],
                      existing_catalog_over_byte_budget=disk["file_bytes"] > limits.max_catalog_bytes)
        if prior and metadata == prior["metadata"] and raw == prior["raw"]:
            if read_current(catalog) != current:
                raise CaptureError("CURRENT changed during capture")
            result.update(status="unchanged", published=False, recompressed_files=0,
                          elapsed_ms=(time.perf_counter() - started) * 1000)
            return result
        if disk["generations"] >= limits.max_generations:
            raise CaptureError("retained generation count budget is full; previous CURRENT retained")
        # Reserve the largest accepted manifest/report and one simultaneous
        # pointer temp file. A too-small budget can conservatively reject a
        # capture whose final report would be smaller than that reservation.
        metadata_reserve = (16 << 10) + (64 << 10)
        candidate_budget = min(limits.max_generation_bytes,
                               limits.max_catalog_bytes - disk["file_bytes"] - 33)
        expected_packed_bytes = 96 + ((len(captured) + 7) // 8) * 128
        fixed_bytes = len(raw) + len(metadata) + expected_packed_bytes
        if fixed_bytes + metadata_reserve + 64 + len(captured) * 24 > candidate_budget:
            raise CaptureError("catalog/generation byte budget cannot admit the candidate and its metadata reservation")
        generation = uuid.uuid4().hex
        stage = catalog / (".stage-" + generation)
        final_directory = generations / generation
        if stage.exists() or final_directory.exists():
            raise CaptureError("new generation collision")
        try:
            stage.mkdir(exist_ok=False)
            _write_exclusive(stage / "features.i32", raw)
            _write_exclusive(stage / "rows.tsv", metadata)
            pack_command = [str(packer), "pack", "--input", str(stage / "features.i32"),
                            "--output", str(stage / "features.sfp"), "--channels", "4",
                            "--block-rows", "64", "--verify"]
            packer_digest_before = hashlib.sha256(_read_stable(packer, 256 << 20)).hexdigest()
            packing = subprocess.run(pack_command, capture_output=True, timeout=120, check=False)
            if packing.returncode:
                raise CaptureError(f"native feature packer failed ({packing.returncode}): {packing.stderr[:4096].decode('utf-8', errors='replace')}")
            if len(packing.stdout) + len(packing.stderr) > 64 << 10:
                raise CaptureError("native packer report exceeds budget")
            if (stage / "features.sfp").stat().st_size != expected_packed_bytes:
                raise CaptureError("native packer file extent differs from the admitted format")
            payload = write_store(stage / "payload.spp", stage / "features.i32", CHANNELS,
                                  captured, max_row_bytes=limits.max_file_bytes,
                                  max_total_decoded_bytes=limits.max_total_decoded_bytes,
                                  max_store_bytes=candidate_budget - fixed_bytes - metadata_reserve)
            component_lines = []
            for name in COMPONENTS:
                path = stage / name
                data = _read_stable(path, limits.max_generation_bytes)
                component_lines.append(f"component\t{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}")
                with open(path, "r+b") as stream:
                    os.fsync(stream.fileno())
            manifest = "\n".join(["SUFCAT01", f"generation\t{generation}",
                                    f"rows\t{len(captured)}", "channels\t4", "schema\t1",
                                    *component_lines]) + "\n"
            if len(manifest) > 16 << 10:
                raise CaptureError("manifest budget exceeded")
            _write_exclusive(stage / "manifest.txt", manifest.encode("ascii"))
            packer_digest_after = hashlib.sha256(_read_stable(packer, 256 << 20)).hexdigest()
            if packer_digest_before != packer_digest_after:
                raise CaptureError("native packer identity changed during generation construction")
            result.update(status="published", published=True, generation=generation,
                          recompressed_files=len(captured), payload_store=payload,
                          packer_sha256=packer_digest_before,
                          packer_command=pack_command,
                          created_utc=datetime.now(timezone.utc).isoformat(),
                          directory_fsync_supported=None,
                          durability="atomic_visibility; power_loss_durability_is_filesystem_and_platform_dependent")
            report_bytes = (json.dumps(result, indent=2, sort_keys=True) + "\n").encode("utf-8")
            if len(report_bytes) > 64 << 10:
                raise CaptureError("capture report budget exceeded")
            _write_exclusive(stage / "capture.json", report_bytes)
            actual_bytes = sum(path.stat().st_size for path in stage.iterdir())
            if actual_bytes > candidate_budget:
                raise CaptureError("complete generation byte budget exceeded")
            final_inventory = _inventory(root, catalog, limits, excludes)
            if not _same_inventory(before, final_inventory):
                raise CaptureError("source inventory changed before publication")
            if read_current(catalog) != current:
                raise CaptureError("CURRENT changed before publication")
            generation_synced = _flush_directory(stage)
            stage.rename(final_directory)
            stage = None
            parent_synced = _flush_directory(generations)
            pointer_temp = catalog / (".CURRENT-" + generation + ".tmp")
            _write_exclusive(pointer_temp, (generation + "\n").encode("ascii"))
            os.replace(pointer_temp, catalog / "CURRENT")
            pointer_temp = None
            committed = True
            result.update(generation_bytes=actual_bytes,
                          elapsed_ms=(time.perf_counter() - started) * 1000)
            try:
                pointer_synced = _flush_directory(catalog)
                result["directory_fsync_supported"] = generation_synced and parent_synced and pointer_synced
            except OSError as error:
                result["post_commit_durability_warning"] = str(error)
            return result
        finally:
            remove_final = False
            if final_directory is not None and not committed:
                # A signal/exception can arrive after os.replace succeeds but
                # before Python records committed=True. Never delete a possibly
                # published generation. Uncertain reads retain an orphan, which
                # is safer than a dangling CURRENT and remains disk-accounted.
                try:
                    remove_final = read_current(catalog) != generation
                except BaseException:
                    remove_final = False
            if pointer_temp is not None:
                pointer_temp.unlink(missing_ok=True)
            if stage is not None:
                try:
                    shutil.rmtree(stage)
                except FileNotFoundError:
                    # Rename may have succeeded immediately before interruption.
                    pass
            if final_directory is not None and remove_final and final_directory.exists():
                # Only this invocation's unpublished, random-ID candidate.
                shutil.rmtree(final_directory)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True, help="built native suffice executable")
    parser.add_argument("--expected-current", help="32 lowercase hex generation; 'none' asserts no published generation")
    parser.add_argument("--exclude", action="append", default=[], help="additional excluded path-component name")
    parser.add_argument("--max-files", type=int, default=100_000)
    parser.add_argument("--max-file-bytes", type=int, default=1 << 20)
    parser.add_argument("--max-total-bytes", type=int, default=128 << 20)
    parser.add_argument("--max-generation-bytes", type=int, default=512 << 20)
    parser.add_argument("--max-catalog-bytes", type=int, default=1 << 30)
    parser.add_argument("--max-generations", type=int, default=16)
    parser.add_argument("--max-metadata-bytes", type=int, default=16 << 20)
    args = parser.parse_args(argv)
    limits = CaptureLimits(max_files=args.max_files, max_file_bytes=args.max_file_bytes,
                           max_total_decoded_bytes=args.max_total_bytes,
                           max_generation_bytes=args.max_generation_bytes,
                           max_catalog_bytes=args.max_catalog_bytes,
                           max_generations=args.max_generations,
                           max_rows_metadata_bytes=args.max_metadata_bytes)
    try:
        result = capture(args.root, args.catalog, args.packer, expected_current=args.expected_current,
                         limits=limits, excludes=DEFAULT_EXCLUDES | frozenset(args.exclude))
    except (OSError, ValueError, CaptureError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "error", "error": str(error)}, ensure_ascii=True), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
