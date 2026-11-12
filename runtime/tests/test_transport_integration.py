#!/usr/bin/env python3
"""Actual producer/native/client boundary checks for complete arbitrary-byte output."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import artifact_catalog
from catalog_client import CatalogError, CatalogProcess

ALL = "const yes 1\noutput yes > 0\n"
NONE = "const no 0\noutput no > 0\n"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def descriptor(rows):
    return [{"row": r["row"], "path": r["path"], "bytes": len(r["data"]),
             "sha256": hashlib.sha256(r["data"]).hexdigest()} for r in rows]


def fingerprint(rows):
    state = 14695981039346656037
    for row in rows:
        name = row["path"].encode("utf-8")
        prefix = struct.pack("<QQ", row["row"], len(name)) + name + struct.pack("<Q", len(row["data"]))
        for group in (prefix, row["data"]):
            for byte in group:
                state = ((state ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return str(state)


def expected_row_wire(response, rows, transport):
    total = 0
    for row in rows:
        name = row["path"].encode("utf-8")
        if transport == "binary":
            total += 48 + len(name) + len(row["data"])
        else:
            envelope = {"type": "row", "seq": response["seq"], "row": row["row"],
                        "path_hex": name.hex(), "data_hex": ""}
            total += len(json.dumps(envelope, separators=(",", ":")).encode("ascii")) + 1 + 2 * len(row["data"])
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source, store = output / "source", output / "store"
    originals = {
        "00_empty.bin": b"",
        "10_all_octets.bin": bytes(range(256)) * 1024,
        "20_one_mib.bin": bytes(range(256)) * 4096,
        "unicode/بيانات_雪.txt": "Unicode path and text: π 雪\r\n".encode("utf-8") + b"\x00\x1a\xff\r\n",
        "z_space name.txt": b"LF\nCRLF\r\nSUB\x1aNUL\x00END",
    }
    for name, body in originals.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
    expected = [{"row": i, "path": name, "data": originals[name]}
                for i, name in enumerate(sorted(originals, key=lambda p: p.encode("utf-8")))]
    byte_count = sum(len(r["data"]) for r in expected)
    tracked = [args.binary.resolve(), args.packer.resolve(), Path(__file__).resolve(),
               ROOT / "scripts/catalog_client.py", ROOT / "scripts/artifact_catalog.py",
               ROOT / "scripts/payload_store.py"]
    before = {str(p): digest(p) for p in tracked}
    counts = {"processes": 0, "complete_output_checks": 0, "verified_rows": 0,
              "verified_bytes": 0, "streaming_checks": 0, "hash_checks": 0,
              "preflight_rejections": 0, "wire_formula_checks": 0, "quit_checks": 0}
    report = {"schema_version": 1, "status": "running", "scope": "Correctness and wire accounting; not a performance experiment",
              "native_windows_tested": os.name == "nt", "inputs_before": before,
              "source_rows": descriptor(expected), "arms": []}
    started = time.perf_counter()
    try:
        report["capture"] = artifact_catalog.capture(source, store, args.packer.resolve())
        wanted_fnv = fingerprint(expected)
        for transport in ("jsonl", "binary"):
            for population in ("capture", "recompute"):
                arm = {"transport": transport, "population": population, "calls": []}
                report["arms"].append(arm)
                client = CatalogProcess(args.binary, transport=transport,
                                        native_options=["--population", population])
                counts["processes"] += 1
                with client:
                    arm["opened"] = client.open("bytes", store, expected_revision=0)

                    def check(label, wanted, **kwargs):
                        response, rows = client.run("bytes", kwargs.pop("program", ALL),
                                                    max_rows=len(wanted),
                                                    max_bytes=sum(len(r["data"]) for r in wanted), **kwargs)
                        assert rows == wanted, label
                        assert response["selected_rows"] == len(wanted)
                        assert response["output_bytes"] == sum(len(r["data"]) for r in wanted)
                        assert response["response_row_wire_bytes"] == expected_row_wire(response, wanted, transport)
                        assert response["response_wire_bytes"] == response["response_row_wire_bytes"] + response["response_control_wire_bytes"]
                        counts["complete_output_checks"] += 1
                        counts["verified_rows"] += len(wanted)
                        counts["verified_bytes"] += sum(len(r["data"]) for r in wanted)
                        counts["wire_formula_checks"] += 1
                        arm["calls"].append({"label": label, "response": response, "rows": descriptor(rows)})
                        return response

                    direct = check("all_arbitrary_bytes", expected, cache="bypass")
                    assert direct["output_fnv64"] == wanted_fnv
                    populated = check("populate_arbitrary_bytes", expected, cache="populate")
                    assert populated["cache_stored"]
                    replay = check("replay_arbitrary_bytes", expected, cache="use")
                    assert replay["cache_hit"]
                    check("full_large_rows_in_range", expected[1:3], first=1, count=2, cache="bypass")
                    check("selected_empty_body_zero_byte_budget", expected[:1], first=0, count=1, cache="populate")
                    check("empty_selection_zero_budgets", [], program=NONE, cache="populate")

                    delivered = []
                    def consume(row):
                        wanted = expected[len(delivered)]
                        assert row == wanted
                        delivered.append({"row": row["row"], "bytes": len(row["data"])})
                    streaming, retained = client.run("bytes", ALL, collect=False, on_row=consume,
                                                     max_rows=len(expected), max_bytes=byte_count)
                    assert retained == [] and len(delivered) == len(expected)
                    assert streaming["response_row_wire_bytes"] == expected_row_wire(streaming, expected, transport)
                    counts["complete_output_checks"] += 1
                    counts["streaming_checks"] += 1
                    counts["verified_rows"] += len(expected)
                    counts["verified_bytes"] += byte_count
                    counts["wire_formula_checks"] += 1
                    arm["calls"].append({"label": "streaming_without_collection", "response": streaming,
                                         "observed": delivered})

                    for label, row_cap, byte_cap in (("row_limit", len(expected) - 1, byte_count),
                                                     ("byte_limit", len(expected), byte_count - 1)):
                        observed = []
                        try:
                            client.run("bytes", ALL, max_rows=row_cap, max_bytes=byte_cap, on_row=observed.append)
                        except CatalogError as error:
                            assert not observed and error.rows == []
                            assert error.result["delivered_rows"] == error.result["delivered_bytes"] == 0
                            counts["preflight_rejections"] += 1
                            arm["calls"].append({"label": label, "response": error.result})
                        else:
                            raise AssertionError("output preflight unexpectedly succeeded")
                        assert client.stats()["status"] == "ok" and not client.closed
                    hashed, rows = client.run("bytes", ALL, emit="hash", max_rows=len(expected), max_bytes=byte_count)
                    assert rows == [] and hashed["output_fnv64"] == wanted_fnv
                    assert hashed["response_row_wire_bytes"] == 0
                    counts["hash_checks"] += 1
                    arm["calls"].append({"label": "hash_diagnostic_same_complete_bytes", "response": hashed})
                assert client.process.returncode == 0 and client.close_result["op"] == "QUIT"
                assert client.close_result["response_row_wire_bytes"] == 0
                counts["quit_checks"] += 1
                arm["close_result"] = client.close_result
                arm["wire_bytes_received"] = client.wire_bytes_received
                arm["wire_bytes_sent"] = client.wire_bytes_sent
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=repr(error), traceback=traceback.format_exc())
        print(report["traceback"], file=sys.stderr)
    finally:
        after = {str(p): digest(p) for p in tracked}
        report.update(inputs_after=after, inputs_stable=before == after, counters=counts,
                      elapsed_ms=(time.perf_counter() - started) * 1000)
        if before != after:
            report.update(status="failed", identity_error="Inputs changed during validation")
        (output / "RESULT.json").write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n")
    print(json.dumps({"status": report["status"], "counters": counts}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
