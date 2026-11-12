#!/usr/bin/env python3
"""Actual-file integration of capture, native admission, generation leases and caches.

All fixtures, native response records, corruption cases and identity records are
retained beneath one new output directory. This is correctness evidence, not a
performance benchmark. Plaintext/query expectations do not use producer features.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import artifact_catalog
from catalog_client import CatalogError, CatalogProcess
from payload_store import write_store


ALL = "const yes 1\noutput yes > 0\n"
NONE = "const no 0\noutput no > 0\n"
SIZE = "input size 0 0\noutput size > 4\n"
TEXT = "input text 3 0\noutput text > 0\n"
PREVIOUS_SIZE = "input previous 0 -1\noutput previous > 4\n"


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def ordered_rows(originals):
    return [{"row": index, "path": name, "data": originals[name]}
            for index, name in enumerate(sorted(originals, key=lambda value: value.encode("utf-8")))]


def selection(originals, kind, first=0, count=None):
    rows = ordered_rows(originals)
    count = len(rows) - first if count is None else count
    answer = []
    for index in range(first, first + count):
        data = rows[index]["data"]
        if kind == "all":
            accepted = True
        elif kind == "none":
            accepted = False
        elif kind == "size":
            accepted = len(data) > 4
        elif kind == "previous_size":
            accepted = len(rows[max(0, index - 1)]["data"]) > 4
        elif kind == "text":
            try:
                data.decode("utf-8", errors="strict")
                accepted = b"\x00" not in data
            except UnicodeDecodeError:
                accepted = False
        else:
            raise AssertionError("unknown independent predicate")
        if accepted:
            answer.append(rows[index])
    return answer


def fingerprint(rows):
    # Secondary check for the explicit hash-output transport; complete-output
    # equality is asserted separately for every hex-output request below.
    state = 14695981039346656037
    for row in rows:
        path = row["path"].encode("utf-8")
        record = (struct.pack("<QQ", row["row"], len(path)) + path
                  + struct.pack("<Q", len(row["data"])) + row["data"])
        for byte in record:
            state = ((state ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return str(state)


class Audit:
    def __init__(self, directory, binary, packer, transport="jsonl"):
        self.directory, self.binary, self.packer = directory, binary, packer
        self.transport = transport
        self.events = (directory / "events.jsonl").open("x", encoding="utf-8")
        self.counts = {"successful_native_commands": 0, "native_rejections": 0,
                       "full_output_calls": 0, "plaintext_rows_compared": 0,
                       "plaintext_bytes_compared": 0, "hash_only_calls": 0,
                       "producer_publications": 0, "producer_reuses": 0,
                       "corruptions_rejected": 0, "client_failure_checks": 0}
        self.groups = []
        self.corruptions = []

    def record(self, label, result, rows=()):
        event = {"label": label, "result": result,
                 "rows": [{"row": row["row"], "path": row["path"],
                           "data_hex": row["data"].hex()} for row in rows]}
        self.events.write(json.dumps(event, ensure_ascii=True, sort_keys=True) + "\n")
        self.events.flush()

    def ok(self, label, operation):
        result = operation()
        self.counts["successful_native_commands"] += 1
        self.record(label, result)
        assert result["status"] == "ok", label
        return result

    def reject(self, label, operation, contains=None):
        try:
            operation()
        except CatalogError as error:
            self.counts["native_rejections"] += 1
            self.record(label, error.result, error.rows)
            assert error.result["status"] == "error", label
            assert error.rows == [], label + " unexpectedly emitted rows"
            assert error.result.get("delivered_rows", 0) == 0, label
            assert error.result.get("delivered_bytes", 0) == 0, label
            if contains:
                assert contains in error.result["error"], (label, error.result)
            return error.result
        raise AssertionError(label + " unexpectedly succeeded")

    def run(self, label, process, project, originals, kind, program, **kwargs):
        expected = selection(originals, kind, kwargs.get("first", 0), kwargs.get("count"))
        result, rows = process.run(project, program, **kwargs)
        self.counts["successful_native_commands"] += 1
        self.record(label, result, rows)
        assert result["selected_rows"] == len(expected), (label, result)
        assert result["output_bytes"] == sum(len(row["data"]) for row in expected), label
        assert result["output_fnv64"] == fingerprint(expected), label
        if kwargs.get("emit", "hex") == "hex":
            assert rows == expected, (label, rows, expected)
            self.counts["full_output_calls"] += 1
            self.counts["plaintext_rows_compared"] += len(expected)
            self.counts["plaintext_bytes_compared"] += result["output_bytes"]
        else:
            assert rows == [], label
            self.counts["hash_only_calls"] += 1
        return result

    def capture(self, label, source, store, expected=None):
        result = artifact_catalog.capture(source, store, self.packer, expected_current=expected)
        self.counts["producer_publications" if result["published"] else "producer_reuses"] += 1
        self.record(label, result)
        return result

    def group(self, name):
        self.groups.append({"name": name, "status": "passed"})


def make_source(directory, contents):
    directory.mkdir(parents=True)
    for name, body in contents.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)


def generation_directory(store):
    generation = (store / "CURRENT").read_text("ascii").rstrip("\n")
    return store / "generations" / generation


def refresh_manifest(directory):
    lines = (directory / "manifest.txt").read_text("ascii").splitlines()
    assert len(lines) == 9
    for index, name in enumerate(("features.i32", "features.sfp", "payload.spp", "rows.tsv"), 5):
        data = (directory / name).read_bytes()
        lines[index] = f"component\t{name}\t{len(data)}\t{hashlib.sha256(data).hexdigest()}"
    (directory / "manifest.txt").write_bytes(("\n".join(lines) + "\n").encode("ascii"))


def replace_rows(directory, mutate):
    fields = [line.split(b"\t") for line in (directory / "rows.tsv").read_bytes().splitlines()]
    mutate(fields)
    (directory / "rows.tsv").write_bytes(b"".join(b"\t".join(row) + b"\n" for row in fields))
    refresh_manifest(directory)


def fixture_workflow(audit):
    base = audit.directory / "fixtures"
    base.mkdir()
    a1 = {"a.rs": b"abcdefghij\n", "b.ts": b"x", "c.bin": b"\xff\x00\n",
          "d.md": b"# docs\nline\n", "empty.py": b"", "line\nname.rs": b"line\n",
          "z.cfg": b"key=value\n", "\u00e9.md": "Unicode \u03bb\n".encode("utf-8")}
    b = {"b.rs": b"B-source\n", "c.json": b'{"b":2}\n', "empty.txt": b""}
    source_a, source_b, source_empty = base / "source_a", base / "source_b", base / "source_empty"
    store_a, store_b, store_empty = base / "catalog_a", base / "catalog_b", base / "catalog_empty"
    make_source(source_a, a1)
    make_source(source_b, b)
    make_source(source_empty, {})
    # Actual excluded build output is present, but absent from the independent
    # expected artifact dictionaries.
    (source_a / "target").mkdir()
    (source_a / "target" / "ignored.bin").write_bytes(b"excluded" * 100)
    capture_a = audit.capture("capture_A1", source_a, store_a, "none")
    capture_b = audit.capture("capture_B", source_b, store_b, "none")
    capture_empty = audit.capture("capture_empty", source_empty, store_empty, "none")
    unchanged = audit.capture("capture_A1_unchanged", source_a, store_a, capture_a["generation"])
    assert not unchanged["published"] and unchanged["generation"] == capture_a["generation"]
    good_a = base / "catalog_a_original"
    shutil.copytree(store_a, good_a)
    cache_storage_a = 0
    initial_charge = {}

    with CatalogProcess(audit.binary, transport=audit.transport) as process:
        unknown = audit.ok("unknown_state", lambda: process.state("A"))
        assert not unknown["loaded"] and unknown["revision"] == 0
        opened_a = audit.ok("open_A1", lambda: process.open("A", store_a, expected_revision=0))
        assert opened_a["generation"] == capture_a["generation"]
        assert opened_a["verified_body_bytes"] == sum(map(len, a1.values()))
        initial_charge = audit.ok("A1_charge", process.stats)["stats"]
        opened_b = audit.ok("open_B", lambda: process.open("B", store_b, expected_revision=0))
        assert opened_b["generation"] == capture_b["generation"]
        opened_empty = audit.ok("open_empty", lambda: process.open("empty", store_empty, expected_revision=0))
        assert opened_empty["generation"] == capture_empty["generation"]
        audit.reject("stale_open_before_missing_path_read",
                     lambda: process.open("A", base / "does_not_exist", expected_revision=0), "stale expected revision before admission")
        assert audit.ok("A_still_current", lambda: process.state("A"))["revision"] == opened_a["revision"]

        populated = audit.run("A_all_populate", process, "A", a1, "all", ALL, cache="populate")
        assert populated["cache_stored"] and not populated["cache_hit"]
        cache_storage_a = populated["cache_storage_bytes"]
        hit = audit.run("A_all_cache_hit", process, "A", a1, "all", ALL, cache="use")
        assert hit["cache_hit"] and hit["decoded_payload_bytes"] == 0 and hit["compressed_payload_bytes"] == 0
        bypass = audit.run("A_all_bypass", process, "A", a1, "all", ALL, cache="bypass")
        assert not bypass["cache_hit"] and bypass["plan_reused"]
        audit.reject("cache_hit_row_cap_preflight", lambda: process.run("A", ALL, cache="use", max_rows=0))
        audit.reject("miss_byte_cap_preflight", lambda: process.run("A", SIZE, cache="use", max_bytes=0))
        audit.run("A_size", process, "A", a1, "size", SIZE, cache="populate")
        audit.run("A_text", process, "A", a1, "text", TEXT, cache="use")
        offset = audit.run("A_global_offset_slice", process, "A", a1, "previous_size", PREVIOUS_SIZE,
                           first=1, count=5, cache="populate")
        assert offset["cache_stored"]
        assert audit.run("A_global_offset_slice_hit", process, "A", a1, "previous_size", PREVIOUS_SIZE,
                         first=1, count=5, cache="use")["cache_hit"]
        assert not audit.run("A_different_range_misses", process, "A", a1, "all", ALL,
                             first=1, count=3, cache="use")["cache_hit"]
        assert not audit.run("A_distinct_source_bytes_miss", process, "A", a1, "all",
                             "# same semantics, distinct exact source\n" + ALL, cache="use")["cache_hit"]
        audit.run("A_hash_transport", process, "A", a1, "all", ALL, emit="hash")
        assert audit.run("B_all_populate", process, "B", b, "all", ALL, cache="populate")["cache_stored"]
        assert audit.run("empty_populate", process, "empty", {}, "all", ALL,
                         cache="populate", max_rows=0, max_bytes=0)["cache_stored"]
        assert audit.run("empty_cache_hit", process, "empty", {}, "all", ALL,
                         max_rows=0, max_bytes=0)["cache_hit"]
        audit.group("exact_complete_output_predicates_ranges_and_cache_identity")

        audit.ok("pin_A1", lambda: process.pin("old_A", "A"))
        a2 = dict(a1)
        del a2["b.ts"]
        del a2["c.bin"]
        a2["00_new.cpp"] = b"new generation\n"
        a2["a.rs"] = b"changed source\n\n"
        a2["renamed.bin"] = a1["c.bin"]
        (source_a / "b.ts").unlink()
        (source_a / "c.bin").rename(source_a / "renamed.bin")
        (source_a / "a.rs").write_bytes(a2["a.rs"])
        (source_a / "00_new.cpp").write_bytes(a2["00_new.cpp"])
        capture_a2 = audit.capture("capture_A2", source_a, store_a, capture_a["generation"])
        opened_a2 = audit.ok("publish_A2", lambda: process.open("A", store_a, expected_revision=opened_a["revision"]))
        assert opened_a2["generation"] == capture_a2["generation"]
        assert opened_a2["revision"] > opened_a["revision"]
        a2_run = audit.run("A2_all_new_row_ids", process, "A", a2, "all", ALL, cache="populate")
        assert not a2_run["cache_hit"] and a2_run["cache_stored"]
        assert audit.run("B_cache_survives_A_update", process, "B", b, "all", ALL)["cache_hit"]
        before_old = audit.ok("retired_charge_before_runs", process.stats)["stats"]
        assert before_old["retired_snapshot_bytes"] == initial_charge["snapshot_bytes"]
        assert before_old["retired_metadata_bytes"] == initial_charge["metadata_bytes"]
        for index in range(2):
            old = audit.run(f"retired_A1_exact_run_{index}", process, "old_A", a1, "all", ALL,
                            pinned=True, cache="populate")
            assert old["retired_generation"] and not old["cache_stored"] and not old["cache_hit"]
            assert not old["plan_reused"]
        after_old = audit.ok("retired_charge_after_runs", process.stats)["stats"]
        for key in ("plan_entries", "plan_text_bytes", "result_entries", "result_bytes",
                    "snapshot_bytes", "metadata_bytes", "retired_snapshot_bytes", "retired_metadata_bytes"):
            assert before_old[key] == after_old[key], (key, before_old, after_old)
        audit.ok("unpin_A1", lambda: process.unpin("old_A"))
        after_unpin = audit.ok("retired_reclaimed", process.stats)["stats"]
        assert after_unpin["retired_snapshot_bytes"] == 0 and after_unpin["retired_metadata_bytes"] == 0
        assert after_unpin["snapshot_bytes"] == after_old["snapshot_bytes"] - initial_charge["snapshot_bytes"]
        audit.reject("old_alias_removed", lambda: process.run("old_A", ALL, pinned=True))
        audit.group("generation_publication_precise_invalidation_old_leases_and_reclamation")

        audit.reject("stale_drop", lambda: process.drop("A", expected_revision=opened_a["revision"]))
        audit.ok("drop_A2", lambda: process.drop("A", expected_revision=opened_a2["revision"]))
        dropped = audit.ok("A_tombstone", lambda: process.state("A"))
        assert not dropped["loaded"] and dropped["revision"] > opened_a2["revision"]
        audit.reject("stale_reopen_after_drop", lambda: process.open("A", store_a, expected_revision=0))
        reopened = audit.ok("reopen_A_from_tombstone", lambda: process.open("A", store_a, expected_revision=dropped["revision"]))
        assert reopened["generation"] == capture_a2["generation"]
        audit.run("reopened_A_exact", process, "A", a2, "all", ALL)
        malformed = [
            ["STATE"], ["UNKNOWN"], ["STATE", "0"], ["STATE", "ff"],
            ["STATE", "41", "extra"], ["STATS", *["x"] * 10], ["STATS\r"], ["STATS\x00"],
            ["RUN", "41", ALL.encode().hex(), "-1", "all", "use", "hex", "10", "100"],
            ["RUN", "41", ALL.encode().hex(), "0", "all", "bad", "hex", "10", "100"],
            ["OPEN", "41", str(store_a).encode().hex(), str(1 << 64)],
        ]
        for index, fields in enumerate(malformed):
            audit.reject(f"protocol_reject_{index}", lambda fields=fields: process.request(fields))
            recovered = audit.ok(f"protocol_recovered_{index}", lambda: process.state("A"))
            assert recovered["revision"] == reopened["revision"]
        audit.group("revision_tombstones_and_protocol_error_recovery")

    return {"good_a": good_a, "store_b": store_b, "empty": store_empty,
            "a": a1, "b": b, "initial_charge": initial_charge,
            "cache_storage_a": cache_storage_a}


def budget_workflow(audit, fixtures):
    a, b = fixtures["a"], fixtures["b"]
    store_a, store_b, empty = fixtures["good_a"], fixtures["store_b"], fixtures["empty"]
    # One global LRU, exercised by two distinct projects.
    with CatalogProcess(audit.binary, transport=audit.transport, native_options=["--max-cache-entries", "2", "--max-cache-bytes", "65536"]) as process:
        audit.ok("LRU_open_A", lambda: process.open("A", store_a, expected_revision=0))
        audit.ok("LRU_open_B", lambda: process.open("B", store_b, expected_revision=0))
        audit.run("LRU_populate_A", process, "A", a, "all", ALL, cache="populate")
        audit.run("LRU_populate_B", process, "B", b, "all", ALL, cache="populate")
        assert audit.run("LRU_touch_A", process, "A", a, "all", ALL)["cache_hit"]
        audit.run("LRU_populate_A_size", process, "A", a, "size", SIZE, cache="populate")
        assert not audit.run("LRU_B_evicted", process, "B", b, "all", ALL)["cache_hit"]
        assert audit.run("LRU_A_retained", process, "A", a, "all", ALL)["cache_hit"]
        stats = audit.ok("LRU_global_stats", process.stats)["stats"]
        assert stats["result_entries"] == 2 and stats["result_bytes"] <= 65536 and stats["cache_evictions"] >= 1
    # Byte pressure evicts across projects independently of the entry limit.
    byte_cap = fixtures["cache_storage_a"]
    with CatalogProcess(audit.binary, transport=audit.transport, native_options=["--max-cache-bytes", str(byte_cap)]) as process:
        audit.ok("byte_LRU_open_A", lambda: process.open("A", store_a, expected_revision=0))
        audit.ok("byte_LRU_open_B", lambda: process.open("B", store_b, expected_revision=0))
        assert audit.run("byte_LRU_populate_A", process, "A", a, "all", ALL, cache="populate")["cache_stored"]
        assert audit.run("byte_LRU_populate_B", process, "B", b, "all", ALL, cache="populate")["cache_stored"]
        stats = audit.ok("byte_LRU_stats", process.stats)["stats"]
        assert stats["result_bytes"] <= byte_cap and stats["result_entries"] == 1
        assert not audit.run("byte_LRU_A_evicted", process, "A", a, "all", ALL)["cache_hit"]
        assert audit.run("byte_LRU_B_retained", process, "B", b, "all", ALL)["cache_hit"]
    with CatalogProcess(audit.binary, transport=audit.transport, native_options=["--max-cache-bytes", "0", "--max-cache-entries", "1"]) as process:
        audit.ok("zero_cache_open_A", lambda: process.open("A", store_a, expected_revision=0))
        audit.ok("zero_cache_open_empty", lambda: process.open("empty", empty, expected_revision=0))
        oversized = audit.run("zero_cache_nonempty_output", process, "A", a, "all", ALL, cache="populate")
        assert oversized["cache_oversized"] and not oversized["cache_stored"]
        zero = audit.run("zero_cache_empty_generation", process, "empty", {}, "all", ALL,
                         cache="populate", max_rows=0, max_bytes=0)
        assert zero["cache_stored"] and zero["cache_storage_bytes"] == 0
        audit.run("zero_cache_false_predicate", process, "A", a, "none", NONE,
                  cache="populate", max_rows=0, max_bytes=0)
        stats = audit.ok("zero_cache_bounded_entries", process.stats)["stats"]
        assert stats["result_entries"] == 1 and stats["result_bytes"] == 0
    audit.group("global_cache_entry_and_byte_pressure_and_zero_byte_results")

    snapshot_cap = fixtures["initial_charge"]["snapshot_bytes"]
    with CatalogProcess(audit.binary, transport=audit.transport, native_options=["--max-snapshot-bytes", str(snapshot_cap)]) as process:
        opened = audit.ok("snapshot_cap_open_A", lambda: process.open("A", store_a, expected_revision=0))
        audit.ok("snapshot_cap_pin_A", lambda: process.pin("old", "A"))
        audit.reject("snapshot_cap_reject_B", lambda: process.open("B", store_b, expected_revision=0))
        audit.ok("snapshot_cap_drop_A", lambda: process.drop("A", expected_revision=opened["revision"]))
        pinned = audit.ok("snapshot_cap_retired_still_charged", process.stats)["stats"]
        assert pinned["snapshot_bytes"] == snapshot_cap and pinned["retired_snapshot_bytes"] == snapshot_cap
        audit.reject("snapshot_cap_pinned_reject_B", lambda: process.open("B", store_b, expected_revision=0))
        audit.run("snapshot_cap_old_lease_exact", process, "old", a, "all", ALL, pinned=True)
        audit.ok("snapshot_cap_unpin", lambda: process.unpin("old"))
        assert audit.ok("snapshot_cap_reclaimed", process.stats)["stats"]["snapshot_bytes"] == 0
        audit.ok("snapshot_cap_B_now_admitted", lambda: process.open("B", store_b, expected_revision=0))
        audit.run("snapshot_cap_B_exact", process, "B", b, "all", ALL)
    metadata_cap = fixtures["initial_charge"]["metadata_bytes"]
    with CatalogProcess(audit.binary, transport=audit.transport, native_options=["--max-metadata-bytes", str(metadata_cap)]) as process:
        audit.ok("metadata_cap_open_A", lambda: process.open("A", store_a, expected_revision=0))
        audit.reject("metadata_cap_reject_B", lambda: process.open("B", store_b, expected_revision=0))
        stats = audit.ok("metadata_cap_stats", process.stats)["stats"]
        assert stats["metadata_bytes"] <= metadata_cap and stats["active_projects"] == 1
        assert not audit.ok("metadata_cap_B_unpublished", lambda: process.state("B"))["loaded"]
    audit.group("snapshot_and_metadata_budgets_include_pinned_retired_owners")


def corruption_workflow(audit, fixtures):
    root = audit.directory / "corruptions"
    root.mkdir()
    cases = []

    def add(name, mutate, expected):
        store = root / name
        shutil.copytree(fixtures["good_a"], store)
        directory = generation_directory(store)
        mutate(store, directory)
        cases.append((name, store, expected))

    def raw_mismatch(_, directory):
        path = directory / "features.i32"
        raw = bytearray(path.read_bytes()); raw[0] ^= 1; path.write_bytes(raw)

    add("raw_component_hash", raw_mismatch, "SHA-256 mismatch")
    add("bad_current", lambda store, _: (store / "CURRENT").write_bytes(b"../outside\n"), "CURRENT")
    add("manifest_field_order", lambda _, directory: (directory / "manifest.txt").write_bytes(
        (directory / "manifest.txt").read_bytes().replace(b"channels\t4\nschema\t1", b"schema\t1\nchannels\t4")), "manifest field/order")
    add("metadata_traversal", lambda _, directory: replace_rows(directory,
        lambda rows: rows[0].__setitem__(1, b"../outside".hex().encode())), "canonical relative paths")
    add("metadata_invalid_utf8", lambda _, directory: replace_rows(directory,
        lambda rows: rows[0].__setitem__(1, b"ff")), "path encoding")

    def unsorted(rows):
        rows[0][1], rows[1][1] = rows[1][1], rows[0][1]

    add("metadata_unsorted", lambda _, directory: replace_rows(directory, unsorted), "strictly byte-sorted")
    add("metadata_duplicate", lambda _, directory: replace_rows(directory,
        lambda rows: rows[1].__setitem__(1, rows[0][1])), "strictly byte-sorted")
    add("metadata_path_over_4096", lambda _, directory: replace_rows(directory,
        lambda rows: rows[0].__setitem__(1, (b"a" * 4097).hex().encode())), "encoded byte cap")
    add("metadata_extra_row", lambda _, directory: replace_rows(directory,
        lambda rows: rows.append([str(len(rows)).encode(), b"extra".hex().encode(), b"0", hashlib.sha256(b"").hexdigest().encode()])), "extra artifact metadata")

    def amplified(_, directory):
        (directory / "rows.tsv").write_bytes(b"\n" * (1 << 20))
        refresh_manifest(directory)

    add("metadata_newline_amplification", amplified, "metadata field")
    add("metadata_body_digest", lambda _, directory: replace_rows(directory,
        lambda rows: rows[0].__setitem__(3, b"0" * 64)), "metadata/body identity mismatch")

    def payload_corrupt(_, directory):
        path = directory / "payload.spp"
        body = bytearray(path.read_bytes()); body[-1] ^= 1; path.write_bytes(body)
        refresh_manifest(directory)

    add("payload_body_corrupt_rehashed", payload_corrupt, None)

    def semantic_corruption(_, directory):
        raw_path = directory / "features.i32"
        raw = bytearray(raw_path.read_bytes())
        original_lf = struct.unpack_from("<i", raw, 8)[0]
        struct.pack_into("<i", raw, 8, original_lf + 1)
        raw_path.write_bytes(raw)
        (directory / "features.sfp").unlink()
        command = [str(audit.packer), "pack", "--input", str(raw_path), "--output", str(directory / "features.sfp"),
                   "--channels", "4", "--block-rows", "64", "--verify"]
        packed = subprocess.run(command, capture_output=True, check=True)
        assert packed.returncode == 0
        (directory / "payload.spp").unlink()
        write_store(directory / "payload.spp", raw_path, 4,
                    [row["data"] for row in ordered_rows(fixtures["a"])])
        refresh_manifest(directory)
        (directory / "semantic_fixture.json").write_text(json.dumps({
            "mutation": "raw feature channel2 LF_count increased by1; native feature repacked and verified; payload regenerated with new raw binding; all manifest hashes refreshed",
            "packer_command": command, "packer_stdout": packed.stdout.decode("utf-8"),
        }, indent=2) + "\n", encoding="utf-8")

    add("valid_checksums_wrong_semantics", semantic_corruption, "feature semantics mismatch")

    symlink_store = root / "symlink_component"
    shutil.copytree(fixtures["good_a"], symlink_store)
    directory = generation_directory(symlink_store)
    component = directory / "features.i32"
    component.rename(directory / "raw_original.i32")
    try:
        component.symlink_to("raw_original.i32")
    except (NotImplementedError, OSError) as error:
        audit.record("symlink_fixture_skipped", {"status": "skipped", "reason": str(error)})
    else:
        cases.append(("symlink_component", symlink_store, "linked"))

    with CatalogProcess(audit.binary, transport=audit.transport) as process:
        opened = audit.ok("corruptions_load_good", lambda: process.open("A", fixtures["good_a"], expected_revision=0))
        baseline = audit.ok("corruptions_baseline_stats", process.stats)["stats"]
        for name, store, expected in cases:
            error = audit.reject("reject_" + name,
                                 lambda store=store: process.open("A", store, expected_revision=opened["revision"]), expected)
            audit.counts["corruptions_rejected"] += 1
            audit.corruptions.append({"name": name, "store": str(store.relative_to(audit.directory)),
                                      "error": error["error"], "status": "rejected"})
            current = audit.ok("unchanged_after_" + name, lambda: process.state("A"))
            assert current["revision"] == opened["revision"] and current["generation"] == opened["generation"]
            now = audit.ok("accounting_after_" + name, process.stats)["stats"]
            for key in ("snapshot_bytes", "metadata_bytes", "active_projects", "publications"):
                assert now[key] == baseline[key], (name, key)
        audit.run("good_output_after_all_corruptions", process, "A", fixtures["a"], "all", ALL)
    audit.group("native_generation_corruption_rejection_and_atomic_failed_admission")


def callback_workflow(audit, fixtures):
    with CatalogProcess(audit.binary, transport=audit.transport) as process:
        audit.ok("callback_open_A", lambda: process.open("A", fixtures["good_a"], expected_revision=0))
        nested_rejections = []

        def nested_callback(row):
            sequence = process.sequence
            try:
                process.state("A")
            except RuntimeError as error:
                nested_rejections.append(str(error))
            else:
                raise AssertionError("nested callback request unexpectedly allowed")
            assert process.sequence == sequence

        result, rows = process.run("A", ALL, on_row=nested_callback)
        audit.counts["successful_native_commands"] += 1
        audit.record("nested_callback_outer_complete", result, rows)
        assert rows == ordered_rows(fixtures["a"])
        assert len(nested_rejections) == len(rows)
        audit.counts["client_failure_checks"] += len(nested_rejections)
        audit.counts["full_output_calls"] += 1
        audit.counts["plaintext_rows_compared"] += len(rows)
        audit.counts["plaintext_bytes_compared"] += sum(len(row["data"]) for row in rows)
        audit.ok("request_after_nested_callback_rejection", lambda: process.state("A"))
        audit.reject("native_error_before_recovery", lambda: process.run("A", ALL, max_rows=0))
        audit.ok("request_after_consumed_native_error", lambda: process.state("A"))
    process = CatalogProcess(audit.binary, transport=audit.transport)
    try:
        audit.ok("callback_error_open_A", lambda: process.open("A", fixtures["good_a"], expected_revision=0))

        def callback_error(row):
            raise CatalogError({"status": "error", "error": "deliberate user callback CatalogError"})

        try:
            process.run("A", ALL, on_row=callback_error)
        except CatalogError as error:
            assert "deliberate user callback" in str(error)
        else:
            raise AssertionError("callback exception unexpectedly swallowed")
        assert process.closed and process.process.poll() is not None
        try:
            process.state("A")
        except RuntimeError:
            pass
        else:
            raise AssertionError("request after callback failure unexpectedly allowed")
        audit.counts["client_failure_checks"] += 1
        audit.record("callback_CatalogError_closed_process", {"status": "passed", "closed": process.closed,
                                                               "returncode": process.process.returncode})
    finally:
        process.close(force=True)
    audit.group("client_reentrancy_and_callback_failure_protocol_cleanup")


def input_inventory(directory):
    answer = {}
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory).as_posix()
        if path.is_symlink():
            answer[relative] = {"kind": "symlink", "target": os.readlink(path)}
        elif path.is_file() and path.name not in {"RESULT.json", "INPUTS.json"}:
            answer[relative] = {"bytes": path.stat().st_size, "sha256": sha(path)}
    return answer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory for complete retained evidence")
    parser.add_argument("--transport", choices=("jsonl", "binary"), default="jsonl")
    args = parser.parse_args()
    binary, packer, output = args.binary.resolve(), args.packer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tracked = [binary, packer, Path(__file__).resolve(), ROOT / "scripts/catalog_client.py",
               ROOT / "scripts/artifact_catalog.py", ROOT / "scripts/payload_store.py"]
    before = {str(path): sha(path) for path in tracked}
    started = time.perf_counter()
    audit = Audit(output, binary, packer, args.transport)
    report = {"schema_version": 1, "status": "running", "validation_kind": "behavioral_integration_not_benchmark",
              "invocation": [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
              "python": sys.version, "transport": args.transport, "native_windows_tested": os.name == "nt", "inputs_before": before}
    try:
        fixtures = fixture_workflow(audit)
        budget_workflow(audit, fixtures)
        corruption_workflow(audit, fixtures)
        callback_workflow(audit, fixtures)
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=repr(error), traceback=traceback.format_exc())
        print(report["traceback"], file=sys.stderr)
    finally:
        audit.events.close()
        after = {str(path): sha(path) for path in tracked}
        report.update(inputs_after=after, inputs_stable=before == after,
                      groups=audit.groups, counters=audit.counts, corruptions=audit.corruptions,
                      elapsed_ms=(time.perf_counter() - started) * 1000)
        if before != after:
            report.update(status="failed", identity_error="binary or script changed during integration")
        with (output / "INPUTS.json").open("x", encoding="utf-8") as stream:
            json.dump(input_inventory(output), stream, indent=2, sort_keys=True)
            stream.write("\n")
        report["input_inventory_sha256"] = sha(output / "INPUTS.json")
        with (output / "RESULT.json").open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    print(json.dumps({"status": report["status"], "groups": len(audit.groups),
                      "counters": audit.counts, "result": str(output / "RESULT.json")}, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
