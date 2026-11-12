#!/usr/bin/env python3
"""Failure-focused client framing checks, independent of the native encoder.

The byte fixtures below spell out the wire layout without importing the client's
frame packer. Fake pipe reads exercise every split/truncation position. A real
child-process check catches the stdout-EOF/open-stderr deadlock specifically.
Native full-output equivalence is covered separately by catalog integration.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import catalog_client as client

COUNTS = {"validated_terminal_responses": 0, "expected_protocol_rejections": 0,
          "fragmentation_positions": 0, "truncation_positions": 0,
          "real_pipe_eof_checks": 0}
PROGRAM = "const yes 1\noutput yes > 0\n"


def scalar(value, width=8):
    return value.to_bytes(width, "little", signed=False)


def frame(kind, payload, *, magic=b"SFC1", flags=0, reserved=0, length=None):
    if length is None:
        length = len(payload)
    return magic + bytes((kind, flags)) + scalar(reserved, 2) + scalar(length) + payload


def row_wire(transport, sequence, row, name, body):
    if isinstance(name, str):
        name = name.encode("utf-8")
    if transport == "binary":
        prefix = b"".join(scalar(value) for value in (sequence, row, len(name), len(body)))
        return frame(2, prefix + name + body)
    event = {"type": "row", "seq": sequence, "row": row,
             "path_hex": name.hex(), "data_hex": body.hex()}
    return json.dumps(event, separators=(",", ":")).encode("ascii") + b"\n"


def control_wire(transport, event):
    payload = json.dumps(event, separators=(",", ":")).encode("utf-8")
    return frame(1, payload) if transport == "binary" else payload + b"\n"


def result(sequence=1, op="RUN", rows=1, size=3, **extra):
    value = {"type": "result", "seq": sequence, "status": "ok", "op": op}
    if op in {"RUN", "RUNPIN"}:
        value.update(selected_rows=rows, output_bytes=size)
    return {**value, **extra}


def error_result(sequence=1, rows=0, size=0, **extra):
    return {"type": "result", "seq": sequence, "status": "error", "error": "expected failure",
            "delivered_rows": rows, "delivered_bytes": size, **extra}


class Reader:
    def __init__(self, payload, *, fragment=None, cut=None):
        self.payload = payload
        self.position = 0
        self.fragment = fragment
        self.cut = cut
        self.closed = False

    def read(self, count):
        if self.closed:
            raise ValueError("read on closed fixture")
        count = min(count, len(self.payload) - self.position)
        if self.fragment is not None:
            count = min(count, self.fragment)
        if self.cut is not None and self.position < self.cut:
            count = min(count, self.cut - self.position)
        data = self.payload[self.position:self.position + count]
        self.position += count
        return data

    def readline(self, count):
        end = self.payload.find(b"\n", self.position, self.position + count)
        end = min(len(self.payload), self.position + count) if end < 0 else end + 1
        value = self.payload[self.position:end]
        self.position = end
        return value

    def close(self):
        self.closed = True


class Writer:
    def __init__(self, fragment=None):
        self.payload = bytearray()
        self.fragment = fragment
        self.closed = False

    def write(self, value):
        count = len(value) if self.fragment is None else min(self.fragment, len(value))
        self.payload.extend(value[:count])
        return count

    def flush(self):
        pass

    def close(self):
        self.closed = True


class NoStderrRead:
    def read(self, count=-1):
        raise AssertionError("client attempted a potentially blocking stderr read")

    def close(self):
        pass


class Process:
    def __init__(self, payload, *, fragment=None, cut=None, write_fragment=None):
        self.stdin = Writer(write_fragment)
        self.stdout = Reader(payload, fragment=fragment, cut=cut)
        self.stderr = NoStderrRead()
        self.returncode = None
        self.killed = False

    def poll(self):
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        if self.returncode is None:
            self.returncode = 0
        return self.returncode


class FramingTests(unittest.TestCase):
    def connection(self, payload, transport="binary", **options):
        process = Process(payload, **options)
        with mock.patch.object(client.subprocess, "Popen", return_value=process) as popen:
            catalog = client.CatalogProcess(sys.executable, transport=transport)
        self.assertEqual(popen.call_args.args[0][1:4], ["serve", "--transport", transport])
        self.addCleanup(catalog.close, force=True)
        return catalog, process

    def rejects(self, wire, transport="binary", *, read_bytes=None, request=None, **options):
        catalog, process = self.connection(wire, transport, **options)
        with self.assertRaises(client.ProtocolError):
            (request or (lambda c: c.run("project", PROGRAM)))(catalog)
        self.assertTrue(catalog.closed)
        self.assertTrue(process.killed)
        if read_bytes is not None:
            self.assertEqual(process.stdout.position, read_bytes)
        COUNTS["expected_protocol_rejections"] += 1
        return catalog, process

    def test_full_bytes_empty_body_and_utf8_path(self):
        expected = [{"row": 0, "path": "目录/δ.bin", "data": bytes(range(256)) * 2},
                    {"row": 1, "path": "empty.txt", "data": b""}]
        for transport in ("jsonl", "binary"):
            with self.subTest(transport=transport):
                row_data = b"".join(row_wire(transport, 1, **{
                    "row": row["row"], "name": row["path"], "body": row["data"]}) for row in expected)
                terminal = control_wire(transport, result(rows=2, size=512))
                catalog, process = self.connection(row_data + terminal, transport, write_fragment=3)
                answer, actual = catalog.run("project", PROGRAM)
                self.assertEqual(actual, expected)
                self.assertEqual(answer["response_wire_bytes"], len(row_data + terminal))
                self.assertEqual(answer["response_row_wire_bytes"], len(row_data))
                self.assertEqual(answer["response_control_wire_bytes"], len(terminal))
                self.assertEqual(answer["request_wire_bytes"], len(process.stdin.payload))
                self.assertEqual(catalog.wire_bytes_sent, len(process.stdin.payload))
                self.assertEqual(catalog.wire_bytes_received, len(row_data + terminal))
                COUNTS["validated_terminal_responses"] += 1

    def test_every_binary_short_read_boundary(self):
        encoded = row_wire("binary", 1, 0, "π.bin", b"\0\r\n\x1a\xff")
        wire = encoded + control_wire("binary", result(size=5))
        for cut in range(1, len(wire)):
            with self.subTest(cut=cut):
                catalog, _ = self.connection(wire, cut=cut)
                answer, rows = catalog.run("P", PROGRAM)
                self.assertEqual(rows[0]["data"], b"\0\r\n\x1a\xff")
                self.assertEqual(answer["response_wire_bytes"], len(wire))
                COUNTS["fragmentation_positions"] += 1
        catalog, _ = self.connection(wire, fragment=1)
        answer, _ = catalog.run("P", PROGRAM)
        self.assertEqual(answer["response_wire_bytes"], len(wire))
        COUNTS["validated_terminal_responses"] += len(wire)

    def test_empty_zero_budget_completion(self):
        for transport in ("jsonl", "binary"):
            terminal = control_wire(transport, result(rows=0, size=0))
            catalog, _ = self.connection(terminal, transport)
            answer, rows = catalog.run("P", PROGRAM, count=0, max_rows=0, max_bytes=0)
            self.assertEqual(rows, [])
            self.assertEqual(answer["response_row_wire_bytes"], 0)
            self.assertEqual(answer["response_control_wire_bytes"], len(terminal))
            COUNTS["validated_terminal_responses"] += 1

    def test_every_binary_truncation_boundary(self):
        wire = row_wire("binary", 1, 0, "x", b"\0\r\n\x1a\xff")
        wire += control_wire("binary", result(size=5))
        for end in range(len(wire)):
            with self.subTest(end=end):
                self.rejects(wire[:end], fragment=3)
                COUNTS["truncation_positions"] += 1

    def test_bad_headers_reject_before_payload(self):
        cases = [frame(2, b"", magic=b"SFC2", length=32),
                 frame(0, b"", length=32), frame(3, b"", length=32),
                 frame(2, b"", flags=1, length=32),
                 frame(2, b"", reserved=1, length=32),
                 frame(2, b"", length=(4 << 20) + 1),
                 frame(1, b"", length=(1 << 64) - 1), frame(2, b"", length=31)]
        for index, wire in enumerate(cases):
            with self.subTest(case=index):
                self.rejects(wire, read_bytes=16)

    def test_bad_nested_lengths_reject_before_body(self):
        for sequence, path_size, body_size, declared in [
                (2, 1, 3, 36), (1, 0, 3, 35), (1, 32769, 3, 32804),
                (1, 1, 4, 36), (1, 1, 2, 36),
                (1, 1, (1 << 64) - 1, 36)]:
            with self.subTest(sequence=sequence, path=path_size, body=body_size, declared=declared):
                prefix = b"".join(scalar(x) for x in (sequence, 0, path_size, body_size))
                self.rejects(frame(2, prefix, length=declared), read_bytes=48)

    def test_invalid_path_encodings(self):
        for transport in ("jsonl", "binary"):
            for path in (b"", b"a\0b", b"\xff", b"\xc0\x80"):
                with self.subTest(transport=transport, path=path):
                    self.rejects(row_wire(transport, 1, 0, path, b"abc"), transport)

    def test_aggregate_byte_limit_before_second_body(self):
        for transport in ("jsonl", "binary"):
            first = row_wire(transport, 1, 0, "a", b"ab")
            second = row_wire(transport, 1, 1, "b", b"cd")
            limit = len(first) + 48 if transport == "binary" else len(first + second)
            self.rejects(first + second, transport, read_bytes=limit,
                         request=lambda c: c.run("P", PROGRAM, max_bytes=3))

    def test_aggregate_row_limit_before_second_payload(self):
        for transport in ("jsonl", "binary"):
            first = row_wire(transport, 1, 0, "a", b"")
            second = row_wire(transport, 1, 1, "b", b"")
            limit = len(first) + 16 if transport == "binary" else len(first + second)
            self.rejects(first + second, transport, read_bytes=limit,
                         request=lambda c: c.run("P", PROGRAM, max_rows=1))

    def test_zero_row_budget_rejects_before_binary_prefix(self):
        self.rejects(row_wire("binary", 1, 0, "a", b""), read_bytes=16,
                     request=lambda c: c.run("P", PROGRAM, max_rows=0))

    def test_row_order_and_range(self):
        for transport in ("jsonl", "binary"):
            self.rejects(row_wire(transport, 1, 2, "a", b"") + row_wire(transport, 1, 2, "b", b""), transport)
            self.rejects(row_wire(transport, 1, 1, "a", b""), transport,
                         request=lambda c: c.run("P", PROGRAM, first=2))
            self.rejects(row_wire(transport, 1, 3, "a", b""), transport,
                         request=lambda c: c.run("P", PROGRAM, first=2, count=1))

    def test_terminal_objects_and_fields(self):
        malformed = [None, [], "result", 0,
                     result(sequence=True), result(sequence=1.0), result(sequence=-1), result(sequence=2),
                     result(sequence=1 << 64), result(status="unknown"), result(status=[]),
                     result(op="STATE"), result(rows=True), result(size=3.0),
                     result(rows=0), result(size=4), result(type="wrong"),
                     error_result(rows=0, size=0), error_result(rows=1, size=3, error=None),
                     error_result(rows=True, size=3)]
        for transport in ("jsonl", "binary"):
            prefix = row_wire(transport, 1, 0, "a", b"abc")
            for index, terminal in enumerate(malformed):
                with self.subTest(transport=transport, case=index):
                    self.rejects(prefix + control_wire(transport, terminal), transport)

    def test_json_duplicates_nonfinite_and_non_utf8(self):
        cases = [b'{"type":"result","seq":1,"seq":1,"status":"ok","op":"STATS"}',
                 b'{"type":"result","seq":1,"status":"ok","op":"STATS","x":NaN}',
                 b'\xff', b'{', b'']
        for transport in ("jsonl", "binary"):
            for raw in cases:
                with self.subTest(transport=transport, raw=raw):
                    wire = frame(1, raw) if transport == "binary" else raw + b"\n"
                    self.rejects(wire, transport, request=lambda c: c.stats())

    def test_json_row_schema_and_hex(self):
        normal = {"type": "row", "seq": 1, "row": 0, "path_hex": "61", "data_hex": "616263"}
        cases = [{**normal, "seq": True}, {**normal, "row": True}, {**normal, "row": 0.0},
                 {**normal, "row": -1}, {**normal, "data_hex": "aa bb"},
                 {**normal, "data_hex": "xx"}, {**normal, "data_hex": "a"},
                 {**normal, "data_hex": []}, {**normal, "path_hex": None},
                 {**normal, "path_hex": "61" * 32769}]
        for event in cases:
            self.rejects(json.dumps(event).encode() + b"\n", "jsonl")

    def test_json_line_cap_and_unterminated_line(self):
        self.rejects(b"x" * ((4 << 20) + 1), "jsonl", read_bytes=(4 << 20) + 1)
        self.rejects(json.dumps(result()).encode(), "jsonl")

    def test_terminal_frame_cannot_hide_row(self):
        event = {"type": "row", "seq": 1, "row": 0, "path_hex": "61", "data_hex": ""}
        self.rejects(control_wire("binary", event))

    def test_native_error_prefix_and_recovery(self):
        for transport in ("jsonl", "binary"):
            row = row_wire(transport, 1, 0, "a", b"abc")
            error = control_wire(transport, error_result(rows=1, size=3))
            state = control_wire(transport, result(sequence=2, op="STATS"))
            catalog, process = self.connection(row + error + state, transport)
            with self.assertRaises(client.CatalogError) as seen:
                catalog.run("P", PROGRAM)
            self.assertEqual(seen.exception.rows, [{"row": 0, "path": "a", "data": b"abc"}])
            self.assertEqual(seen.exception.result["response_wire_bytes"], len(row + error))
            self.assertFalse(catalog.closed)
            self.assertFalse(process.killed)
            self.assertEqual(catalog.stats()["seq"], 2)
            self.assertEqual(catalog.wire_bytes_received, len(row + error + state))
            COUNTS["validated_terminal_responses"] += 2

    def test_stream_only_counts_independent_of_collection(self):
        for transport in ("jsonl", "binary"):
            wire = row_wire(transport, 1, 0, "a", b"abc")
            wire += control_wire(transport, result())
            catalog, _ = self.connection(wire, transport)
            received = []
            answer, rows = catalog.run("P", PROGRAM, collect=False, on_row=received.append)
            self.assertEqual(rows, [])
            self.assertEqual(len(received), answer["selected_rows"])
            self.assertEqual(received[0]["data"], b"abc")
            COUNTS["validated_terminal_responses"] += 1

    def test_callback_catalog_error_closes_connection(self):
        for transport in ("jsonl", "binary"):
            wire = row_wire(transport, 1, 0, "a", b"abc") + control_wire(transport, result())
            catalog, process = self.connection(wire, transport)
            def callback(row):
                raise client.CatalogError({"error": "callback failure"})
            with self.assertRaisesRegex(client.CatalogError, "callback failure"):
                catalog.run("P", PROGRAM, on_row=callback)
            self.assertTrue(catalog.closed)
            self.assertTrue(process.killed)
            self.assertLess(process.stdout.position, len(wire))

    def test_reentrancy_rejects_before_sequence_write(self):
        for transport in ("jsonl", "binary"):
            wire = row_wire(transport, 1, 0, "a", b"abc") + control_wire(transport, result())
            catalog, process = self.connection(wire, transport)
            def catches_nested(row):
                before = bytes(process.stdin.payload)
                with self.assertRaisesRegex(RuntimeError, "not reentrant"):
                    catalog.stats()
                self.assertEqual(catalog.sequence, 1)
                self.assertEqual(bytes(process.stdin.payload), before)
            catalog.run("P", PROGRAM, on_row=catches_nested)
            self.assertFalse(catalog.closed)
            COUNTS["validated_terminal_responses"] += 1
            second, process2 = self.connection(wire, transport)
            with self.assertRaisesRegex(RuntimeError, "not reentrant"):
                second.run("P", PROGRAM, on_row=lambda row: second.stats())
            self.assertTrue(second.closed)
            self.assertTrue(process2.killed)

    def test_hash_mode_has_no_rows_but_validates_counts(self):
        for transport in ("jsonl", "binary"):
            catalog, _ = self.connection(control_wire(transport, result(rows=2, size=8)), transport)
            answer, rows = catalog.run("P", PROGRAM, emit="hash", max_rows=2, max_bytes=8)
            self.assertEqual(rows, [])
            self.assertEqual(answer["selected_rows"], 2)
            COUNTS["validated_terminal_responses"] += 1
            failed, process = self.connection(control_wire(transport, error_result(rows=1, size=3)), transport)
            with self.assertRaises(client.CatalogError) as seen:
                failed.run("P", PROGRAM, emit="hash", max_rows=2, max_bytes=8)
            self.assertEqual(seen.exception.rows, [])
            self.assertEqual(seen.exception.result["delivered_rows"], 1)
            self.assertFalse(failed.closed)
            self.assertFalse(process.killed)
            COUNTS["validated_terminal_responses"] += 1
            self.rejects(control_wire(transport, result(rows=2, size=9)), transport,
                         request=lambda c: c.run("P", PROGRAM, emit="hash", max_bytes=8))
            self.rejects(control_wire(transport, error_result(rows=1, size=9)), transport,
                         request=lambda c: c.run("P", PROGRAM, emit="hash", max_bytes=8))
            self.rejects(row_wire(transport, 1, 0, "a", b"abc"), transport,
                         request=lambda c: c.run("P", PROGRAM, emit="hash"))
            self.rejects(row_wire(transport, 1, 0, "a", b"abc"), transport,
                         request=lambda c: c.stats())

    def test_malformed_raw_requests_accept_consumed_native_error(self):
        requests = [["RUN", "50", PROGRAM.encode().hex(), "-1", "all", "use", "hex", "10", "100"],
                    ["RUN", "50", PROGRAM.encode().hex(), "0", "all", "bad", "hex", "10", "100"],
                    ["OPEN", "50", "61", str(1 << 64)], ["STATS\0"], ["STATE", "50", "extra"]]
        for transport in ("jsonl", "binary"):
            for fields in requests:
                catalog, process = self.connection(control_wire(transport, error_result()), transport)
                with self.assertRaises(client.CatalogError):
                    catalog.request(fields)
                self.assertFalse(catalog.closed)
                self.assertFalse(process.killed)
                COUNTS["validated_terminal_responses"] += 1

    def test_hash_mode_respects_explicit_count(self):
        for transport in ("jsonl", "binary"):
            for count in (0, 1):
                for terminal in (result(rows=count + 1, size=0), error_result(rows=count + 1, size=0)):
                    with self.subTest(transport=transport, count=count, status=terminal["status"]):
                        self.rejects(control_wire(transport, terminal), transport,
                                     request=lambda c, count=count: c.run("P", PROGRAM, count=count,
                                         emit="hash", max_rows=10, max_bytes=0))
            catalog, _ = self.connection(control_wire(transport, result(rows=1, size=0)), transport)
            answer, rows = catalog.run("P", PROGRAM, count=1, emit="hash", max_rows=10, max_bytes=0)
            self.assertEqual(rows, [])
            self.assertEqual(answer["selected_rows"], 1)
            COUNTS["validated_terminal_responses"] += 1

    def test_close_retains_quit_and_cumulative_counters(self):
        for transport in ("jsonl", "binary"):
            first = control_wire(transport, result(op="STATS"))
            quit_wire = control_wire(transport, result(sequence=2, op="QUIT"))
            catalog, process = self.connection(first + quit_wire, transport)
            catalog.stats()
            catalog.close()
            self.assertTrue(catalog.closed)
            self.assertFalse(process.killed)
            self.assertEqual(catalog.close_result["op"], "QUIT")
            self.assertEqual(catalog.close_result["response_wire_bytes"], len(quit_wire))
            self.assertEqual(catalog.wire_bytes_received, len(first + quit_wire))
            self.assertEqual(catalog.wire_bytes_sent, len(b"STATS\nQUIT\n"))
            COUNTS["validated_terminal_responses"] += 2

    def test_transport_option_conflicts_do_not_spawn(self):
        with mock.patch.object(client.subprocess, "Popen") as popen:
            for options in (["--transport", "binary"], ["--transport=jsonl"]):
                with self.assertRaises(ValueError):
                    client.CatalogProcess(sys.executable, native_options=options)
            with self.assertRaises(ValueError):
                client.CatalogProcess(sys.executable, transport="other")
            popen.assert_not_called()

    def test_real_stdout_eof_does_not_drain_live_stderr(self):
        for transport in ("jsonl", "binary"):
            completed = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                        "--eof-probe", transport],
                                       capture_output=True, timeout=5)
            self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
            COUNTS["real_pipe_eof_checks"] += 1


def eof_probe(transport):
    real_popen = subprocess.Popen
    # This child closes stdout but deliberately keeps stderr/process alive.
    # The outer test has a five-second timeout to expose the original deadlock.
    code = "import os,sys,time;sys.stdin.buffer.readline();os.close(1);time.sleep(20)"
    def child(*args, **kwargs):
        return real_popen([sys.executable, "-u", "-c", code],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with mock.patch.object(client.subprocess, "Popen", side_effect=child):
        catalog = client.CatalogProcess(sys.executable, transport=transport)
    started = time.monotonic()
    try:
        catalog.stats()
    except client.ProtocolError:
        assert catalog.closed and catalog.process.poll() is not None
        assert time.monotonic() - started < 4
    else:
        raise AssertionError("EOF did not invalidate the connection")
    finally:
        catalog.close(force=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--eof-probe", choices=("jsonl", "binary"))
    args = parser.parse_args()
    if args.eof_probe:
        eof_probe(args.eof_probe)
        return 0
    if args.json is not None and args.json.exists():
        parser.error("refusing to overwrite an existing validation result")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FramingTests)
    result_set = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"schema_version": 1, "status": "passed" if result_set.wasSuccessful() else "failed",
              "test_cases": result_set.testsRun, "failures": len(result_set.failures),
              "errors": len(result_set.errors), "skipped": len(result_set.skipped),
              "counters": COUNTS, "python": sys.version, "native_windows_tested": os.name == "nt",
              "client_sha256": hashlib.sha256((ROOT / "scripts/catalog_client.py").read_bytes()).hexdigest(),
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "scope": "Client parser/connection tests with independent framed fixtures and real Python child EOF probes; native encoding/output equivalence is a separate integration gate."}
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if result_set.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
