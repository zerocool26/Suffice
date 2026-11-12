#!/usr/bin/env python3
"""Persistent native artifact-catalog client and JSONL job runner.

Keep one CatalogProcess alive for a session to amortize admission and planning.
The native service is a child process using stdin/stdout, with no network port.
Artifact output is bounded by caller-supplied row/byte limits. Its memory is
separate from native retained-snapshot and result-cache budgets.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import time


MAX_FRAME_PAYLOAD = 4 << 20
MAX_PATH_BYTES = 32768
_FRAME_HEADER = struct.Struct("<4sBBHQ")
_ROW_HEADER = struct.Struct("<QQQQ")
_UINT64_MAX = (1 << 64) - 1


class CatalogError(RuntimeError):
    def __init__(self, result, rows=()):
        super().__init__(result.get("error", "catalog request failed"))
        self.result = result
        self.rows = list(rows)


class ProtocolError(RuntimeError):
    """A malformed/incomplete response; the owned connection is no longer usable."""


def _response_uint(value, name):
    if type(value) is not int or not 0 <= value <= _UINT64_MAX:
        raise ProtocolError("native " + name + " must be uint64")
    return value


def _decimal_uint(value):
    """Recognize the native unsigned-decimal grammar without unbounded int conversion."""
    if not isinstance(value, str) or not value or not value.isascii() or not value.isdecimal():
        return None
    significant = value.lstrip("0") or "0"
    if len(significant) > 20:
        return None
    result = int(significant)
    return result if result <= _UINT64_MAX else None


def _json_object(payload):
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ProtocolError("duplicate native JSON key")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ProtocolError("nonfinite native JSON number")

    try:
        event = json.loads(payload.decode("utf-8", "strict"),
                           object_pairs_hook=unique_pairs, parse_constant=invalid_constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise ProtocolError("invalid native JSON response") from error
    if not isinstance(event, dict):
        raise ProtocolError("native response must be a JSON object")
    return event


def _path_bytes(value):
    if not value or len(value) > MAX_PATH_BYTES or b"\0" in value:
        raise ProtocolError("native row path has invalid length or NUL")
    try:
        return value.decode("utf-8", "strict")
    except UnicodeError as error:
        raise ProtocolError("native row path is not UTF-8") from error


def _unhex(value, name, maximum):
    if not isinstance(value, str) or len(value) % 2 or len(value) // 2 > maximum:
        raise ProtocolError("native " + name + " has invalid encoded extent")
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise ProtocolError("native " + name + " is not hexadecimal") from error
    if len(decoded) * 2 != len(value):
        raise ProtocolError("native " + name + " contains hex whitespace")
    return decoded


def _run_context(fields):
    # Raw request() remains useful for negative protocol checks. Infer output
    # obligations only from a complete valid RUN envelope; malformed requests
    # can still receive a recoverable native error with a zero delivered prefix.
    if len(fields) != 9 or fields[0] not in {"RUN", "RUNPIN"}:
        return None
    first, rows, size = (_decimal_uint(fields[index]) for index in (3, 7, 8))
    count = None if fields[4] == "all" else _decimal_uint(fields[4])
    if (first is None or rows is None or size is None
            or (fields[4] != "all" and count is None)
            or fields[5] not in {"bypass", "use", "populate"}
            or fields[6] not in {"hex", "hash"}):
        return None
    return {"mode": fields[6], "rows": rows, "bytes": size,
            "first": first, "count": count}


def _hex(value):
    if not isinstance(value, str) or "\0" in value:
        raise ValueError("protocol text must be a NUL-free string")
    return value.encode("utf-8", errors="strict").hex()


def _uint(value):
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise ValueError("protocol integer must be uint64")
    return str(value)


class CatalogProcess:
    def __init__(self, binary, *, native_options=(), transport="jsonl"):
        if transport not in {"jsonl", "binary"}:
            raise ValueError("transport must be jsonl or binary")
        options = tuple(map(str, native_options))
        if any(value == "--transport" or value.startswith("--transport=") for value in options):
            raise ValueError("select transport through the client transport argument")
        self.binary = str(Path(binary).resolve())
        self.transport = transport
        self.process = subprocess.Popen([self.binary, "serve", "--transport", transport, *options],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.sequence = 0
        self.closed = False
        self.active = False
        # Application pipe bytes actually written/read, including framing and
        # the implicit QUIT exchange. They remain available after close().
        self.wire_bytes_sent = 0
        self.wire_bytes_received = 0
        self.close_result = None

    def _read_exact(self, count, component):
        if not count:
            return b""
        chunk = self.process.stdout.read(count)
        self.wire_bytes_received += len(chunk)
        if not chunk:
            raise ProtocolError("native stdout ended during " + component)
        if len(chunk) == count:
            return chunk
        collected = bytearray(chunk)
        while len(collected) < count:
            chunk = self.process.stdout.read(count - len(collected))
            self.wire_bytes_received += len(chunk)
            if not chunk:
                raise ProtocolError("native stdout ended during " + component)
            collected.extend(chunk)
        return bytes(collected)

    def _read_event(self, remaining_rows, remaining_bytes, allow_rows):
        if self.transport == "jsonl":
            line = self.process.stdout.readline(MAX_FRAME_PAYLOAD + 1)
            self.wire_bytes_received += len(line)
            if not line:
                # Do not drain stderr while the child is alive: stdout can
                # close independently, leaving a blocking stderr read hanging.
                raise ProtocolError("native stdout ended before a terminal result")
            if len(line) > MAX_FRAME_PAYLOAD or not line.endswith(b"\n"):
                raise ProtocolError("native response line exceeds limit or lacks LF")
            event = _json_object(line)
            if _response_uint(event.get("seq"), "sequence") != self.sequence:
                raise ProtocolError("native response sequence mismatch")
            if event.get("type") == "row":
                if not allow_rows or not remaining_rows:
                    raise ProtocolError("unexpected row or request row limit exceeded")
                row_id = _response_uint(event.get("row"), "row")
                name = _path_bytes(_unhex(event.get("path_hex"), "path", MAX_PATH_BYTES))
                body = _unhex(event.get("data_hex"), "body", remaining_bytes)
                return {"type": "row", "row": row_id, "path": name, "data": body}
            if event.get("type") != "result":
                raise ProtocolError("unknown native response type")
            return event

        header = self._read_exact(_FRAME_HEADER.size, "frame header")
        magic, kind, flags, reserved, length = _FRAME_HEADER.unpack(header)
        if magic != b"SFC1" or kind not in (1, 2) or flags or reserved:
            raise ProtocolError("unsupported binary frame header")
        if length > MAX_FRAME_PAYLOAD:
            raise ProtocolError("binary frame exceeds payload limit")
        if kind == 1:
            event = _json_object(self._read_exact(length, "terminal JSON"))
            if event.get("type") != "result":
                raise ProtocolError("binary terminal frame is not a result")
            if _response_uint(event.get("seq"), "sequence") != self.sequence:
                raise ProtocolError("native response sequence mismatch")
            return event
        if not allow_rows or not remaining_rows:
            raise ProtocolError("unexpected row or request row limit exceeded")
        if length < _ROW_HEADER.size:
            raise ProtocolError("binary row frame is smaller than its metadata")
        sequence, row_id, path_size, body_size = _ROW_HEADER.unpack(
            self._read_exact(_ROW_HEADER.size, "row metadata"))
        if sequence != self.sequence:
            raise ProtocolError("native response sequence mismatch")
        if (not 0 < path_size <= MAX_PATH_BYTES or body_size > remaining_bytes
                or path_size + body_size != length - _ROW_HEADER.size):
            raise ProtocolError("binary row extents exceed limits or disagree with frame")
        name = _path_bytes(self._read_exact(path_size, "row path"))
        body = self._read_exact(body_size, "row body")
        return {"type": "row", "row": row_id, "path": name, "data": body}

    def _write_request(self, wire):
        view = memoryview(wire)
        sent = 0
        while sent < len(wire):
            count = self.process.stdin.write(view[sent:])
            if type(count) is not int or not 0 < count <= len(wire) - sent:
                raise ProtocolError("native request pipe did not accept bytes")
            sent += count
            self.wire_bytes_sent += count
        self.process.stdin.flush()

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.close(force=kind is not None)

    def request(self, fields, *, collect=True, on_row=None):
        if self.closed:
            raise RuntimeError("catalog process is closed")
        if self.active:
            raise RuntimeError("catalog client requests are not reentrant")
        fields = tuple(fields)
        wire = ("\t".join(fields) + "\n").encode("ascii")
        if len(wire) > (256 << 10) + 1:
            raise ValueError("request exceeds native protocol line limit")
        if self.sequence == _UINT64_MAX:
            raise RuntimeError("catalog request sequence exhausted")
        context = _run_context(fields)
        allow_rows = context is not None and context["mode"] == "hex"
        row_limit = context["rows"] if context is not None else 0
        byte_limit = context["bytes"] if context is not None else 0
        self.sequence += 1
        started = time.perf_counter()
        rows = []
        observed_rows = observed_bytes = row_wire_bytes = 0
        previous_row = None
        wire_before = self.wire_bytes_received
        terminal_error = False
        self.active = True
        try:
            self._write_request(wire)
            while True:
                before_event = self.wire_bytes_received
                event = self._read_event(row_limit - observed_rows,
                                         byte_limit - observed_bytes, allow_rows)
                event_wire_bytes = self.wire_bytes_received - before_event
                if event["type"] == "row":
                    row = {key: event[key] for key in ("row", "path", "data")}
                    if (previous_row is not None and row["row"] <= previous_row
                            or row["row"] < context["first"]
                            or context["count"] is not None
                            and row["row"] - context["first"] >= context["count"]):
                        raise ProtocolError("native row order or requested range mismatch")
                    previous_row = row["row"]
                    observed_rows += 1
                    observed_bytes += len(row["data"])
                    row_wire_bytes += event_wire_bytes
                    if collect:
                        rows.append(row)
                    if on_row is not None:
                        on_row(row)
                else:
                    status = event.get("status")
                    if not isinstance(status, str) or status not in {"ok", "error"}:
                        raise ProtocolError("invalid native terminal status")
                    if status == "error":
                        if not isinstance(event.get("error"), str):
                            raise ProtocolError("native terminal error lacks error text")
                        delivered_rows = _response_uint(event.get("delivered_rows"), "delivered rows")
                        delivered_bytes = _response_uint(event.get("delivered_bytes"), "delivered bytes")
                        if context is not None and context["mode"] == "hash":
                            if (delivered_rows > row_limit or delivered_bytes > byte_limit
                                    or context["count"] is not None and delivered_rows > context["count"]):
                                raise ProtocolError("native hash error prefix exceeds request limits")
                        elif (delivered_rows, delivered_bytes) != (observed_rows, observed_bytes):
                            raise ProtocolError("native error prefix differs from delivered rows")
                    else:
                        if not fields or event.get("op") != fields[0]:
                            raise ProtocolError("native terminal operation mismatch")
                        if fields[0] in {"RUN", "RUNPIN"}:
                            if context is None:
                                raise ProtocolError("native accepted an invalid run envelope")
                            selected = _response_uint(event.get("selected_rows"), "selected rows")
                            output = _response_uint(event.get("output_bytes"), "output bytes")
                            if (selected > row_limit or output > byte_limit
                                    or context["count"] is not None and selected > context["count"]):
                                raise ProtocolError("native terminal output exceeds request limits")
                            if allow_rows and (selected, output) != (observed_rows, observed_bytes):
                                raise ProtocolError("native result counts differ from delivered rows")
                    event["request_wire_bytes"] = len(wire)
                    event["response_wire_bytes"] = self.wire_bytes_received - wire_before
                    event["response_row_wire_bytes"] = row_wire_bytes
                    event["response_control_wire_bytes"] = event_wire_bytes
                    event["client_wall_ms"] = (time.perf_counter() - started) * 1000
                    if status == "error":
                        terminal_error = True
                        raise CatalogError(event, rows)
                    return event, rows
        except CatalogError:
            # Only a native terminal error leaves the protocol synchronized.
            # A callback can raise this same exception type before that point.
            if not terminal_error:
                self.close(force=True)
            raise
        except BaseException:
            # An interrupted callback or malformed response must not leave the
            # next request consuming a previous request's rows.
            self.close(force=True)
            raise
        finally:
            self.active = False

    def state(self, project):
        return self.request(["STATE", _hex(project)])[0]

    def open(self, project, catalog_root, *, expected_revision):
        return self.request(["OPEN", _hex(project), _hex(str(Path(catalog_root).resolve())),
                             _uint(expected_revision)])[0]

    def pin(self, alias, project):
        return self.request(["PIN", _hex(alias), _hex(project)])[0]

    def unpin(self, alias):
        return self.request(["UNPIN", _hex(alias)])[0]

    def run(self, project, program, *, first=0, count=None, cache="use", emit="hex",
            max_rows=1 << 20, max_bytes=16 << 20, pinned=False, collect=True, on_row=None):
        if cache not in {"bypass", "use", "populate"} or emit not in {"hex", "hash"}:
            raise ValueError("invalid cache or emit mode")
        return self.request(["RUNPIN" if pinned else "RUN", _hex(project), _hex(program),
                             _uint(first), "all" if count is None else _uint(count), cache,
                             emit, _uint(max_rows), _uint(max_bytes)], collect=collect, on_row=on_row)

    def drop(self, project, *, expected_revision):
        return self.request(["DROP", _hex(project), _uint(expected_revision)])[0]

    def stats(self):
        return self.request(["STATS"])[0]

    def clear(self, target="all"):
        if target not in {"results", "workers", "plans", "all"}:
            raise ValueError("invalid clear target")
        return self.request(["CLEAR", target])[0]

    def close(self, force=False):
        if self.closed:
            return
        try:
            if not force and self.process.poll() is None:
                self.close_result = self.request(["QUIT"])[0]
        except Exception:
            force = True
        finally:
            self.closed = True
            if force and self.process.poll() is None:
                self.process.kill()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--jobs", type=Path, required=True,
                        help="JSONL operations: open,run,state,pin,unpin,drop,stats,clear")
    parser.add_argument("--max-snapshot-bytes", type=int, default=256 << 20)
    parser.add_argument("--max-cache-bytes", type=int, default=64 << 20)
    parser.add_argument("--transport", choices=("jsonl", "binary"), default="jsonl")
    args = parser.parse_args()
    if args.jobs.stat().st_size > 8 << 20:
        parser.error("job file exceeds 8MiB")
    native = ["--max-snapshot-bytes", _uint(args.max_snapshot_bytes),
              "--max-cache-bytes", _uint(args.max_cache_bytes)]

    def output_row(row):
        print(json.dumps({"type": "row", "row": row["row"], "path": row["path"],
                          "data_hex": row["data"].hex()}, ensure_ascii=True), flush=True)

    failed = False
    with CatalogProcess(args.binary, native_options=native, transport=args.transport) as catalog:
        with args.jobs.open("rb") as stream:
            for index, raw in enumerate(stream, 1):
                if not raw.strip():
                    continue
                try:
                    job = json.loads(raw)
                    if not isinstance(job, dict):
                        raise ValueError("job must be an object")
                    op = job.pop("op")
                    if op == "open":
                        result = catalog.open(**job)
                    elif op == "run":
                        if "program_file" in job:
                            p = Path(job.pop("program_file"))
                            if p.stat().st_size > 64 << 10:
                                raise ValueError("program exceeds64KiB")
                            job["program"] = p.read_text(encoding="utf-8")
                        result, _ = catalog.run(**job, collect=False, on_row=output_row)
                    elif op in {"state", "pin", "unpin", "drop", "stats", "clear"}:
                        result = getattr(catalog, op)(**job)
                    else:
                        raise ValueError("unknown job op")
                    print(json.dumps({"job": index, **result}), flush=True)
                except CatalogError as error:
                    failed = True
                    print(json.dumps({"job": index, **error.result}), flush=True)
                except (ValueError, TypeError, KeyError, OSError) as error:
                    failed = True
                    print(json.dumps({"job": index, "status": "error", "error": str(error)}), flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
