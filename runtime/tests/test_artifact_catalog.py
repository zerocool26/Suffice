#!/usr/bin/env python3
"""Behavioral publisher checks using real packed features and native Zstandard.

Run: python3 tests/test_artifact_catalog.py --packer /absolute/path/to/suffice
The plaintext oracle below does not call producer feature/hash helpers.
"""

import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import artifact_catalog as catalog
import zstandard


PACKER = None
COUNTERS = {"row_bytes_compared": 0, "rows_compared": 0, "publications": 0,
            "no_change_reuses": 0, "expected_rejections": 0,
            "interrupted_commits_preserved": 0, "uncertain_candidates_retained": 0}


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="suffice-artifact-test-")
        self.base = Path(self.temp.name)
        self.root = self.base / "source"
        self.root.mkdir()
        self.output = self.base / "catalog"

    def tearDown(self):
        self.temp.cleanup()

    def capture(self, **kwargs):
        result = catalog.capture(self.root, self.output, PACKER, **kwargs)
        COUNTERS["publications" if result["published"] else "no_change_reuses"] += 1
        return result

    def reject(self, callback, exc=(catalog.CaptureError, OSError, ValueError)):
        with self.assertRaises(exc):
            callback()
        COUNTERS["expected_rejections"] += 1

    def decode_generation(self, expected):
        pointer = (self.output / "CURRENT").read_bytes()
        self.assertRegex(pointer, rb"\A[0-9a-f]{32}\n\Z")
        generation = pointer[:-1].decode("ascii")
        folder = self.output / "generations" / generation
        manifest = (folder / "manifest.txt").read_text("ascii").splitlines()
        self.assertEqual(manifest[:5], ["SUFCAT01", "generation\t" + generation,
                                      "rows\t" + str(len(expected)), "channels\t4", "schema\t1"])
        self.assertEqual(len(manifest), 9)
        for line, name in zip(manifest[5:], ("features.i32", "features.sfp", "payload.spp", "rows.tsv")):
            parts = line.split("\t")
            content = (folder / name).read_bytes()
            self.assertEqual(parts, ["component", name, str(len(content)), hashlib.sha256(content).hexdigest()])
        raw = (folder / "features.i32").read_bytes()
        packed = (folder / "features.sfp").read_bytes()
        self.assertGreater(len(packed), 0)
        store = (folder / "payload.spp").read_bytes()
        magic, version, flags, rows, channels, entry_size, _, index_size, store_size, _ = struct.unpack_from("<8sIIQIIQQQQ", store)
        self.assertEqual((magic, version, flags, rows, channels, entry_size, index_size, store_size),
                         (b"SUFPAY01", 1, 1, len(expected), 4, 24, len(expected) * 24, len(store)))
        metadata = (folder / "rows.tsv").read_bytes().splitlines()
        self.assertEqual(len(metadata), len(expected))
        ordered = sorted(expected, key=lambda text: text.encode("utf-8"))
        decoder = zstandard.ZstdDecompressor()
        for index, name in enumerate(ordered):
            original, features = expected[name]
            fields = metadata[index].split(b"\t")
            self.assertEqual(fields, [str(index).encode(), name.encode("utf-8").hex().encode(),
                                      str(len(original)).encode(), hashlib.sha256(original).hexdigest().encode()])
            offset, compressed, decoded, _ = struct.unpack_from("<QIIQ", store, 64 + 24 * index)
            result = decoder.decompress(store[offset:offset + compressed], max_output_size=max(1, decoded))
            self.assertEqual(result, original)
            self.assertEqual(decoded, len(original))
            self.assertEqual(struct.unpack_from("<4i", raw, index * 16), features)
            COUNTERS["rows_compared"] += 1
            COUNTERS["row_bytes_compared"] += len(original)
        self.assertEqual(len(raw), len(expected) * 16)
        return folder

    def test_complete_plaintext_features_unicode_empty_and_exclusions(self):
        expected = {
            "main.RS": (b"fn main() {}\n", (13, 1, 1, 1)),
            "include/a.CUH": (b"// x\n", (5, 2, 1, 1)),
            "caf\u00e9.ts": ("\u03bb\n".encode(), (3, 3, 1, 1)),
            "empty.py": (b"", (0, 4, 0, 1)),
            ".json": (b"{}", (2, 0, 0, 1)),
            "binary.bin": (b"\xff\x00\n", (3, 0, 1, 0)),
            "nul.txt": (b"a\x00b", (3, 8, 0, 0)),
            "badutf8.txt": (b"\xc0\xaf", (2, 8, 0, 0)),
            "line\nbreak.md": (b"# doc\n", (6, 6, 1, 1)),
        }
        for name, (data, _) in expected.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for name in ("target", "node_modules", ".git", "build", ".venv"):
            path = self.root / name
            path.mkdir()
            (path / "ignored.dat").write_bytes(b"X" * 200)
        result = self.capture(expected_current="none")
        self.assertEqual(result["rows"], len(expected))
        self.assertEqual(result["skipped_entries"], 5)
        self.assertEqual(result["added_files"], len(expected))
        self.decode_generation(expected)

    def test_no_change_and_touch_reuse_exact_generation(self):
        file = self.root / "a.rs"
        file.write_bytes(b"x\n")
        first = self.capture()
        current = first["generation"]
        directory = self.output / "generations" / current
        hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}
        second = self.capture(expected_current=current)
        self.assertEqual(second["status"], "unchanged")
        self.assertEqual(second["generation"], current)
        info = file.stat()
        os.utime(file, ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000_000))
        third = self.capture(expected_current=current)
        self.assertEqual(third["generation"], current)
        self.assertEqual(third["recompressed_files"], 0)
        self.assertEqual(third["file_bytes_rehashed"], 2)
        self.assertEqual(hashes, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()})
        self.assertEqual(len(list((self.output / "generations").iterdir())), 1)

    def test_content_change_add_delete_rename_and_old_generation_preservation(self):
        (self.root / "a.rs").write_bytes(b"old")
        (self.root / "gone.txt").write_bytes(b"gone")
        (self.root / "stay.py").write_bytes(b"stay")
        first = self.capture()
        old_path = self.output / "generations" / first["generation"]
        original = {p.name: p.read_bytes() for p in old_path.iterdir()}
        (self.root / "a.rs").write_bytes(b"new\n")
        (self.root / "gone.txt").unlink()
        (self.root / "added.json").write_bytes(b"{}")
        second = self.capture(expected_current=first["generation"])
        self.assertNotEqual(second["generation"], first["generation"])
        self.assertEqual((second["unchanged_files"], second["added_files"], second["deleted_files"], second["modified_files"]), (1, 1, 1, 1))
        self.assertEqual(original, {p.name: p.read_bytes() for p in old_path.iterdir()})
        (self.root / "stay.py").rename(self.root / "renamed.py")
        third = self.capture(expected_current=second["generation"])
        self.assertEqual((third["added_files"], third["deleted_files"], third["modified_files"]), (1, 1, 0))
        self.decode_generation({"a.rs": (b"new\n", (4, 1, 1, 1)),
                                "added.json": (b"{}", (2, 5, 0, 1)),
                                "renamed.py": (b"stay", (4, 4, 0, 1))})

    def test_catalog_inside_source_is_excluded(self):
        self.output = self.root / "captured"
        (self.root / "a.md").write_bytes(b"a")
        first = self.capture()
        second = self.capture()
        self.assertEqual(first["rows"], 1)
        self.assertEqual(second["status"], "unchanged")
        self.assertEqual(second["skipped_entries"], 1)
        self.decode_generation({"a.md": (b"a", (1, 6, 0, 1))})

    def test_case_distinct_catalog_sibling_is_captured(self):
        self.output = self.root / "Catalog"
        source_directory = self.root / "catalog"
        self.output.mkdir()
        try:
            source_directory.mkdir()
        except FileExistsError:
            self.skipTest("filesystem does not support case-distinct sibling directories")
        (source_directory / "a.rs").write_bytes(b"source")
        result = self.capture()
        self.assertEqual(result["rows"], 1)
        self.assertEqual(result["skipped_entries"], 1)
        self.decode_generation({"catalog/a.rs": (b"source", (6, 1, 0, 1))})

    def test_empty_tree_publishes_and_reuses_zero_rows(self):
        first = self.capture()
        self.assertEqual(first["rows"], 0)
        self.decode_generation({})
        self.assertEqual(self.capture()["generation"], first["generation"])

    def test_full_feature_block_and_tail_with_exact_byte_limits(self):
        expected = {}
        for index in range(65):
            name = f"shaders/{index:03d}.glsl"
            data = (b"x\n" * index) + b"\x00\xff"
            expected[name] = (data, (2 * index + 2, 7, index, 0))
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        total = sum(len(data) for data, _ in expected.values())
        limits = replace(catalog.CaptureLimits(), max_files=65,
                         max_file_bytes=130, max_total_decoded_bytes=total)
        result = self.capture(limits=limits)
        self.assertEqual(result["decoded_payload_bytes"], total)
        self.decode_generation(expected)
        self.reject(lambda: self.capture(limits=replace(limits, max_file_bytes=129)))
        self.reject(lambda: self.capture(limits=replace(limits, max_total_decoded_bytes=total - 1)))

    def test_stale_expectation_and_live_lock_preserve_current(self):
        (self.root / "a.txt").write_bytes(b"a")
        first = self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        self.reject(lambda: self.capture(expected_current="none"))
        self.reject(lambda: self.capture(expected_current="0" * 32))
        lock = self.output / ".publish.lock"
        lock.write_bytes(b"other writer\n")
        self.reject(lambda: self.capture())
        self.assertEqual(lock.read_bytes(), b"other writer\n")
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        lock.unlink()
        self.assertEqual(self.capture(expected_current=first["generation"])["status"], "unchanged")

    def test_budgets_fail_without_pointer_change(self):
        (self.root / "a.txt").write_bytes(b"1234")
        self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        base = catalog.CaptureLimits()
        for limits in (replace(base, max_files=0), replace(base, max_file_bytes=3),
                       replace(base, max_total_decoded_bytes=3), replace(base, max_rows_metadata_bytes=3),
                       replace(base, max_generation_bytes=1), replace(base, max_entries=0),
                       replace(base, max_directories=0)):
            self.reject(lambda limits=limits: self.capture(limits=limits))
            self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        self.assertFalse(list(self.output.glob(".stage-*")))
        self.assertFalse((self.output / ".publish.lock").exists())

    def test_same_size_content_change_with_restored_mtime_is_detected(self):
        file = self.root / "a.txt"
        file.write_bytes(b"OLD")
        first = self.capture()
        stamp = file.stat()
        file.write_bytes(b"NEW")
        os.utime(file, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        second = self.capture()
        self.assertNotEqual(first["generation"], second["generation"])
        self.assertEqual(second["modified_files"], 1)
        self.decode_generation({"a.txt": (b"NEW", (3, 8, 0, 1))})

    def test_source_change_after_read_is_rejected(self):
        file = self.root / "a.rs"
        file.write_bytes(b"original")
        first = self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        original = catalog._read_stable
        mutated = False

        def read_and_mutate(path, limit, expected=None):
            nonlocal mutated
            result = original(path, limit, expected)
            if path == file and expected is not None and not mutated:
                mutated = True
                file.write_bytes(b"changed later")
            return result

        with mock.patch.object(catalog, "_read_stable", side_effect=read_and_mutate):
            self.reject(lambda: self.capture(expected_current=first["generation"]))
        self.assertTrue(mutated)
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)

    def test_packer_and_precommit_failures_leave_previous_current(self):
        file = self.root / "a.rs"
        file.write_bytes(b"a")
        self.capture()
        file.write_bytes(b"updated")
        pointer = (self.output / "CURRENT").read_bytes()
        generations = {path.name for path in (self.output / "generations").iterdir()}
        failed_process = type("Process", (), {"returncode": 17, "stderr": b"intentional", "stdout": b""})()
        with mock.patch.object(catalog.subprocess, "run", return_value=failed_process):
            self.reject(lambda: self.capture())
        with mock.patch.object(catalog.os, "replace", side_effect=OSError("injected pointer replace failure")):
            self.reject(lambda: self.capture())
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        self.assertEqual({path.name for path in (self.output / "generations").iterdir()}, generations)
        self.assertFalse(list(self.output.glob(".stage-*")))
        self.assertFalse(list(self.output.glob(".CURRENT-*")))

    def test_post_commit_flush_failure_reports_published_state(self):
        (self.root / "a.rs").write_bytes(b"a")
        original = catalog._flush_directory

        def flush(path):
            if path == self.output and (path / "CURRENT").exists():
                raise OSError("injected post-commit flush failure")
            return original(path)

        with mock.patch.object(catalog, "_flush_directory", side_effect=flush):
            result = self.capture()
        self.assertTrue(result["published"])
        self.assertIn("post_commit_durability_warning", result)
        self.assertEqual(catalog.read_current(self.output), result["generation"])
        persisted = json.loads((self.output / "generations" / result["generation"] / "capture.json").read_text())
        self.assertIsNone(persisted["directory_fsync_supported"])
        self.decode_generation({"a.rs": (b"a", (1, 1, 0, 1))})

    def test_interrupt_immediately_after_pointer_replace_keeps_current_generation(self):
        file = self.root / "a.rs"
        file.write_bytes(b"old")
        first = self.capture()
        file.write_bytes(b"newvalue")
        replace_pointer = catalog.os.replace

        def commit_then_interrupt(source, destination):
            replace_pointer(source, destination)
            raise KeyboardInterrupt("injected immediately after successful pointer replacement")

        with mock.patch.object(catalog.os, "replace", side_effect=commit_then_interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.capture()
        current = catalog.read_current(self.output)
        self.assertNotEqual(current, first["generation"])
        self.assertTrue((self.output / "generations" / current).is_dir())
        self.decode_generation({"a.rs": (b"newvalue", (8, 1, 0, 1))})
        self.assertFalse((self.output / ".publish.lock").exists())
        COUNTERS["interrupted_commits_preserved"] += 1

    def test_uncertain_post_replace_state_retains_unpublished_candidate(self):
        file = self.root / "a.rs"
        file.write_bytes(b"old")
        first = self.capture()
        file.write_bytes(b"newvalue")
        read_pointer = catalog.read_current
        attempted = False

        def fail_replace(source, destination):
            nonlocal attempted
            attempted = True
            raise OSError("injected replacement uncertainty")

        def fail_cleanup_read(path):
            if attempted:
                raise OSError("injected unreadable CURRENT during cleanup")
            return read_pointer(path)

        with mock.patch.object(catalog.os, "replace", side_effect=fail_replace), \
                mock.patch.object(catalog, "read_current", side_effect=fail_cleanup_read):
            self.reject(lambda: self.capture())
        self.assertEqual(catalog.read_current(self.output), first["generation"])
        self.assertEqual(len(list((self.output / "generations").iterdir())), 2)
        self.assertFalse(list(self.output.glob(".stage-*")))
        COUNTERS["uncertain_candidates_retained"] += 1

    def test_windows_direntry_identity_is_refreshed_from_path(self):
        file = self.root / "a.rs"
        file.write_bytes(b"identity")
        entry = mock.Mock()
        entry.stat.side_effect = AssertionError("Windows DirEntry identity must not be used")
        metadata = catalog._entry_metadata(entry, file, windows=True)
        with file.open("rb") as stream:
            self.assertEqual(catalog._stamp(metadata), catalog._stamp(os.fstat(stream.fileno())))
        entry.stat.assert_not_called()

    @unittest.skipIf(os.name == "nt" or not hasattr(os, "mkfifo"), "POSIX FIFO race check")
    def test_raced_fifo_is_rejected_without_blocking_open(self):
        file = self.root / "a.rs"
        file.write_bytes(b"original")
        expected = catalog._stamp(file.stat())
        file.unlink()
        os.mkfifo(file)
        child = """import json,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import artifact_catalog
try:
    artifact_catalog._read_stable(Path(sys.argv[2]),1024,tuple(json.loads(sys.argv[3])))
except artifact_catalog.CaptureError:
    sys.exit(0)
raise AssertionError('FIFO was admitted')
"""
        result = subprocess.run([sys.executable, "-c", child, str(ROOT / "scripts"),
                                 str(file), json.dumps(expected)], capture_output=True,
                                timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
        COUNTERS["expected_rejections"] += 1

    def test_symlinks_and_noncanonical_paths_rejected(self):
        (self.root / "a.rs").write_bytes(b"a")
        self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        outside = self.base / "outside.txt"
        outside.write_bytes(b"outside")
        linked = self.root / "linked.txt"
        try:
            linked.symlink_to(outside)
        except (NotImplementedError, OSError):
            self.skipTest("symlink creation unavailable")
        self.reject(lambda: self.capture())
        linked.unlink()
        linked.symlink_to(self.base, target_is_directory=True)
        self.reject(lambda: self.capture())
        linked.unlink()
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        for value in ("../x", "/x", "a//b", "a/./b", "a\\b", "C:x", "a\0b", "x" * 4097):
            self.reject(lambda value=value: catalog.validate_relative_path(value))
        if os.name != "nt":
            bad = self.root / "a:b"
            bad.write_bytes(b"bad")
            self.reject(lambda: self.capture())

    def test_corrupt_prior_metadata_or_component_rejected(self):
        (self.root / "a.py").write_bytes(b"x")
        result = self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        path = self.output / "generations" / result["generation"] / "payload.spp"
        data = path.read_bytes()
        path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        self.reject(lambda: self.capture())
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        path.write_bytes(data)
        current = self.output / "CURRENT"
        current.write_bytes(b"../arbitrary\n")
        self.reject(lambda: self.capture())
        self.assertEqual(current.read_bytes(), b"../arbitrary\n")

    def test_full_catalog_allows_no_change_but_rejects_growth(self):
        file = self.root / "a.txt"
        file.write_bytes(b"a")
        first = self.capture()
        pointer = (self.output / "CURRENT").read_bytes()
        limits = replace(catalog.CaptureLimits(), max_generations=1)
        self.assertEqual(self.capture(limits=limits)["status"], "unchanged")
        # An explicitly reduced byte budget also permits a zero-growth reuse.
        byte_limits = replace(catalog.CaptureLimits(), max_catalog_bytes=0)
        self.assertEqual(self.capture(limits=byte_limits)["status"], "unchanged")
        file.write_bytes(b"different")
        self.reject(lambda: self.capture(limits=limits))
        self.reject(lambda: self.capture(limits=byte_limits))
        self.assertEqual((self.output / "CURRENT").read_bytes(), pointer)
        self.assertEqual(len(list((self.output / "generations").iterdir())), 1)
        self.assertEqual(catalog.read_current(self.output), first["generation"])

    def test_orphan_staging_bytes_are_charged_without_deletion(self):
        (self.root / "a.txt").write_bytes(b"a")
        self.capture()
        orphan = self.output / (".stage-" + "a" * 32)
        orphan.mkdir()
        marker = orphan / "captured.bin"
        marker.write_bytes(b"x" * 1000)
        reuse = self.capture()
        self.assertGreaterEqual(reuse["catalog_retained_bytes_before"], 1000)
        (self.root / "a.txt").write_bytes(b"changed")
        limits = replace(catalog.CaptureLimits(), max_catalog_bytes=1000)
        self.reject(lambda: self.capture(limits=limits))
        self.assertEqual(marker.read_bytes(), b"x" * 1000)

    def test_extension_schema_and_utf8_edge_cases(self):
        mapping = {"x.rs": 1, "X.CXX": 2, "x.tsx": 3, "x.pyi": 4, "x.XML": 5,
                   "x.markdown": 6, "x.metal": 7, "x.log": 8, "x.data": 0,
                   ".json": 0, "dir.with.dot/noext": 0, "x.\u0130NI": 0}
        for path, expected in mapping.items():
            self.assertEqual(catalog.extension_class(path), expected)
        cases = [(b"", 1), (b"\x00", 0), (b"\xf4\x8f\xbf\xbf", 1),
                 (b"\xf4\x90\x80\x80", 0), (b"\xed\xa0\x80", 0), (b"\xc0\x80", 0)]
        for data, expected in cases:
            self.assertEqual(catalog.derive_features("x.txt", data)[3], expected)


def main():
    global PACKER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packer", required=True, type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    PACKER = args.packer.absolute()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PublisherTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"status": "passed" if result.wasSuccessful() else "failed",
              "test_cases": result.testsRun, "failures": len(result.failures),
              "errors": len(result.errors), "skipped": len(result.skipped),
              "counters": COUNTERS, "packer": str(PACKER),
              "packer_sha256": hashlib.sha256(PACKER.read_bytes()).hexdigest(),
              "producer_sha256": hashlib.sha256((ROOT / "scripts/artifact_catalog.py").read_bytes()).hexdigest(),
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "python": sys.version, "native_windows_tested": os.name == "nt"}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        with args.json.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    print(json.dumps(report, indent=2))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
