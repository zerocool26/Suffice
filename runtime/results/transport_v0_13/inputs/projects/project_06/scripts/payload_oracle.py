"""Independent, bounded input accounting for the 0.10 payload experiment.

This module imports neither the workload generator nor its predicates/checksums.
It derives facts from retained raw features, event identities, the payload index,
and original plaintext. It does not decompress frames: native full admission and
the native byte-for-byte output comparison provide that separate check.
"""
from array import array
import hashlib
import json
from pathlib import Path
import struct

_MAX_ROWS = 1 << 20
_MAX_ROW_BYTES = 1 << 20
_MAX_STORE_BYTES = 512 << 20
_MAX_REFERENCE_BYTES = 600 << 20
_MASK64 = (1 << 64) - 1
_FNV_OFFSET = 14695981039346656037
_FNV_PRIME = 1099511628211


def _require(condition, message):
    if not condition:
        raise ValueError("payload oracle: " + message)


def _integer(value, low, high, name):
    _require(type(value) is int and low <= value <= high, name)
    return value


def _fnv(data, state=_FNV_OFFSET):
    for byte in data:
        state = ((state ^ byte) * _FNV_PRIME) & _MASK64
    return state


def _read(stream, length, message):
    data = stream.read(length)
    _require(len(data) == length, message)
    return data


def _exact_file(path, length, maximum):
    _require(0 <= length <= maximum, "file length exceeds budget")
    _require(path.is_file() and path.stat().st_size == length,
             "file extent differs: " + path.name)
    with path.open("rb") as stream:
        data = _read(stream, length, "truncated " + path.name)
        _require(not stream.read(1), "file grew: " + path.name)
    return data


def _selected(values, query, threshold):
    if query in ("and2", "and8"):
        return all(value > threshold for value in values)
    return ((values[0] > threshold or values[1] > threshold) and
            (values[2] > threshold or values[3] > threshold))


def _decision(low, high, query, threshold):
    # Interval evaluation of min / max is monotone in every input endpoint.
    if query in ("and2", "and8"):
        lower, upper = min(low), min(high)
    else:
        lower = min(max(low[0], low[1]), max(low[2], low[3]))
        upper = min(max(high[0], high[1]), max(high[2], high[3]))
    if lower > threshold:
        return 1  # True
    if upper <= threshold:
        return 0  # False
    return 2  # Unknown


def inspect_case(folder, case):
    """Return independently derived JSON facts; reject malformed retained input.

    selected_ids_sha256 hashes increasing global row IDs as little-endian u64.
    canonical_record_sha256 hashes records sorted by canonical event ID, each
    encoded as event-ID u64, exact little-endian feature row, SHA256(plaintext).
    Compare canonical hashes only for equal row/channel/query/payload cases.
    Zone decisions cover complete physical blocks; zone row counts intersect the
    requested [first, first + count) range. All byte counts are logical bytes.
    """
    folder = Path(folder)
    rows = _integer(case["rows"], 0, _MAX_ROWS, "row count")
    query = case["query"]
    _require(query in ("and2", "and8", "overlap4"), "query family")
    channels = _integer(case["channels"], 1, 8, "channels")
    _require(channels == {"and2": 2, "and8": 8, "overlap4": 4}[query],
             "query/channel mismatch")
    threshold = _integer(case["threshold"], -(1 << 31), (1 << 31) - 1,
                         "threshold")
    first = _integer(case["first"], 0, rows, "first row")
    count = _integer(case["count"], 0, rows - first, "range count")
    end = first + count
    stride = channels * 4
    raw = _exact_file(folder / "features.i32", rows * stride, _MAX_ROWS * 32)
    ids = _exact_file(folder / "event_ids.u64", rows * 8, _MAX_ROWS * 8)
    event_rows = array("Q", [0]) * rows
    _require(event_rows.itemsize == 8, "unsupported uint64 array width")
    seen = bytearray(rows)
    for row in range(rows):
        event = struct.unpack_from("<Q", ids, row * 8)[0]
        _require(event < rows and not seen[event], "event IDs are not a permutation")
        if case.get("layout") == "corpus":
            _require(event == row, "corpus event order differs")
        seen[event] = 1
        event_rows[event] = row

    packed_path = folder / "features.sfp"
    _require(packed_path.is_file() and 64 <= packed_path.stat().st_size <= _MAX_STORE_BYTES,
             "packed extent")
    packed_sha = hashlib.sha256()
    with packed_path.open("rb") as packed:
        header = _read(packed, 64, "packed header truncated")
        packed_sha.update(header)
        magic, version, metadata, packed_rows, packed_channels, block_rows = struct.unpack_from("<8sIIQII", header)
        _require(magic == b"SUFFICE1" and version == 1, "packed format")
        _require(packed_rows == rows and packed_channels == channels, "packed dimensions")
        _require(32 <= block_rows <= 65536 and block_rows & (block_rows - 1) == 0,
                 "packed block rows")
        _require(metadata == 64 + channels * 8, "packed metadata extent")
        full, tail = divmod(rows, block_rows)
        expected_packed_bytes = metadata + (full * (block_rows // 8) + (tail + 7) // 8) * channels * 32
        _require(struct.unpack_from("<Q", header, 32)[0] == expected_packed_bytes - metadata,
                 "packed payload extent")
        consumed = 64
        while chunk := packed.read(1 << 20):
            consumed += len(chunk)
            _require(consumed <= expected_packed_bytes, "packed trailing bytes")
            packed_sha.update(chunk)
        _require(consumed == expected_packed_bytes, "packed file truncated")

    accepted = bytearray(rows)
    global_low = [(1 << 31) - 1] * channels
    global_high = [-(1 << 31)] * channels
    zone_blocks = []
    zone_counts = [0, 0, 0]
    selected_ids = hashlib.sha256()
    selected_rows = 0
    feature_format = "<" + "i" * channels
    for begin in range(0, rows, block_rows):
        stop = min(begin + block_rows, rows)
        low = [(1 << 31) - 1] * channels
        high = [-(1 << 31)] * channels
        for row in range(begin, stop):
            values = struct.unpack_from(feature_format, raw, row * stride)
            for channel, value in enumerate(values):
                low[channel] = min(low[channel], value)
                high[channel] = max(high[channel], value)
            if first <= row < end and _selected(values, query, threshold):
                accepted[row] = 1
                selected_rows += 1
                selected_ids.update(struct.pack("<Q", row))
        for channel in range(channels):
            global_low[channel] = min(global_low[channel], low[channel])
            global_high[channel] = max(global_high[channel], high[channel])
        decision = _decision(low, high, query, threshold)
        zone_blocks.append(decision)
        zone_counts[decision] += max(0, min(stop, end) - max(begin, first))
    if not rows:
        # The packed format uses [0,0] channel ranges for an empty dataset.
        global_low = global_high = [0] * channels
    if "selected_rows" in case:
        _require(type(case["selected_rows"]) is int and case["selected_rows"] == selected_rows,
                 "recorded selected count differs")

    store_path = folder / "payload.spp"
    _require(store_path.is_file() and 64 <= store_path.stat().st_size <= _MAX_STORE_BYTES,
             "store extent")
    store_sha = hashlib.sha256()
    with store_path.open("rb") as store:
        header = _read(store, 64, "store header truncated")
        store_sha.update(header)
        fields = struct.unpack("<8sIIQIIQQQQ", header)
        magic, version, codec, store_rows, store_channels, entry_bytes, raw_fnv, index_bytes, store_bytes, index_fnv = fields
        _require((magic, version, codec, store_rows, store_channels, entry_bytes) ==
                 (b"SUFPAY01", 1, 1, rows, channels, 24), "store identity")
        _require(index_bytes == rows * 24 and 64 + index_bytes <= store_bytes <= _MAX_STORE_BYTES,
                 "store index extent")
        _require(store_bytes == store_path.stat().st_size, "declared store extent")
        _require(raw_fnv == _fnv(raw), "feature/store binding")
        index = _read(store, index_bytes, "store index truncated")
        store_sha.update(index)
        _require(index_fnv == _fnv(index, _fnv(header[:56] + bytes(8))),
                 "store index checksum")
        cursor = 64 + index_bytes
        total_compressed = total_decoded = 0
        selected_compressed = range_compressed = range_decoded = 0
        for row in range(rows):
            offset, compressed, decoded, _ = struct.unpack_from("<QIIQ", index, row * 24)
            _require(offset == cursor and 4 <= compressed <= store_bytes - cursor,
                     "noncontiguous or invalid frame extent")
            _require(decoded <= _MAX_ROW_BYTES, "decoded row exceeds budget")
            magic = _read(store, 4, "frame magic truncated")
            _require(magic == b"\x28\xb5\x2f\xfd", "not an ordinary Zstandard frame")
            store_sha.update(magic)
            left = compressed - 4
            while left:
                chunk = _read(store, min(left, 1 << 16), "frame truncated")
                store_sha.update(chunk)
                left -= len(chunk)
            cursor += compressed
            total_compressed += compressed
            total_decoded += decoded
            _require(total_decoded <= 4 << 30, "decoded total exceeds budget")
            if first <= row < end:
                range_compressed += compressed
                range_decoded += decoded
            if accepted[row]:
                selected_compressed += compressed
        _require(cursor == store_bytes and not store.read(1), "store trailing bytes")

    corpus_records = None
    corpus_sha = None
    if case.get("layout") == "corpus":
        corpus_path = folder / "corpus.json"
        _require(corpus_path.is_file() and corpus_path.stat().st_size <= 8 << 20,
                 "corpus manifest extent")
        corpus_bytes = _exact_file(corpus_path, corpus_path.stat().st_size, 8 << 20)
        corpus = json.loads(corpus_bytes)
        _require(corpus["features"] == ["byte_count", "64 * newline_count"],
                 "corpus feature definition")
        corpus_records = corpus["records"]
        _require(isinstance(corpus_records, list) and len(corpus_records) == rows,
                 "corpus record count")
        paths = [entry["path"] for entry in corpus_records]
        _require(all(isinstance(path, str) for path in paths) and len(set(paths)) == rows,
                 "corpus source inventory")
        corpus_sha = hashlib.sha256(corpus_bytes).hexdigest()

    reference_path = folder / "plaintext.ref"
    _require(reference_path.is_file() and 16 <= reference_path.stat().st_size <= _MAX_REFERENCE_BYTES,
             "reference extent")
    reference_sha = hashlib.sha256()
    plaintext_payloads_sha = hashlib.sha256()
    payload_digests = bytearray(rows * 32)
    output_fnv = _FNV_OFFSET
    output_bytes = decoded_from_reference = 0
    reference_bytes = 16
    with reference_path.open("rb") as reference:
        header = _read(reference, 16, "reference header truncated")
        reference_sha.update(header)
        _require(header[:8] == b"SUFREF01" and struct.unpack_from("<Q", header, 8)[0] == rows,
                 "reference identity")
        for row in range(rows):
            size_bytes = _read(reference, 4, "reference row length truncated")
            reference_sha.update(size_bytes)
            length = struct.unpack("<I", size_bytes)[0]
            _require(length <= _MAX_ROW_BYTES, "reference row exceeds budget")
            _require(length == struct.unpack_from("<I", index, row * 24 + 12)[0],
                     "index/reference decoded length differs")
            reference_bytes += 4 + length
            _require(reference_bytes <= _MAX_REFERENCE_BYTES, "reference exceeds budget")
            decoded_from_reference += length
            plaintext_payloads_sha.update(struct.pack("<Q", length))
            if accepted[row]:
                output_bytes += length
                output_fnv = _fnv(struct.pack("<QQ", row, length), output_fnv)
            payload_sha = hashlib.sha256()
            payload_fnv = _FNV_OFFSET
            newlines = 0
            left = length
            while left:
                chunk = _read(reference, min(left, 1 << 16), "reference row truncated")
                reference_sha.update(chunk)
                plaintext_payloads_sha.update(chunk)
                payload_sha.update(chunk)
                payload_fnv = _fnv(chunk, payload_fnv)
                if accepted[row]:
                    output_fnv = _fnv(chunk, output_fnv)
                if corpus_records is not None:
                    newlines += chunk.count(b"\n")
                left -= len(chunk)
            _require(payload_fnv == struct.unpack_from("<Q", index, row * 24 + 16)[0],
                     "index/reference decoded checksum differs")
            event = struct.unpack_from("<Q", ids, row * 8)[0]
            payload_digests[event * 32:(event + 1) * 32] = payload_sha.digest()
            if corpus_records is not None:
                entry = corpus_records[row]
                _require(entry["row"] == row and entry["bytes"] == length and
                         entry["newlines"] == newlines and entry["sha256"] == payload_sha.hexdigest(),
                         "corpus source record differs")
                _require(struct.unpack_from(feature_format, raw, row * stride) == (length, newlines * 64),
                         "corpus features differ from plaintext")
        _require(not reference.read(1), "reference trailing bytes")
    _require(decoded_from_reference == total_decoded, "total decoded length differs")

    canonical = hashlib.sha256()
    for event in range(rows):
        row = event_rows[event]
        canonical.update(struct.pack("<Q", event))
        canonical.update(raw[row * stride:(row + 1) * stride])
        canonical.update(payload_digests[event * 32:(event + 1) * 32])
    result = {
        "schema_version": 10,
        "rows": rows, "channels": channels, "query": query, "threshold": threshold,
        "first": first, "count": count,
        "selected_rows": selected_rows,
        "selected_ids_sha256": selected_ids.hexdigest(),
        "output_bytes": output_bytes, "output_fnv64": str(output_fnv),
        "selected_compressed_bytes": selected_compressed,
        "selected_decoded_bytes": output_bytes,
        "total_compressed_bytes": total_compressed,
        "total_decoded_bytes": total_decoded,
        "range_compressed_bytes": range_compressed,
        "range_decoded_bytes": range_decoded,
        "block_rows": block_rows,
        "constant_decision": _decision(global_low, global_high, query, threshold),
        "zone_blocks": zone_blocks,
        "zone_false_rows": zone_counts[0], "zone_true_rows": zone_counts[1],
        "zone_refined_rows": zone_counts[2],
        "canonical_record_sha256": canonical.hexdigest(),
        "raw_sha256": hashlib.sha256(raw).hexdigest(),
        "event_ids_sha256": hashlib.sha256(ids).hexdigest(),
        "packed_sha256": packed_sha.hexdigest(), "store_sha256": store_sha.hexdigest(),
        "plaintext_sha256": reference_sha.hexdigest(),
        "plaintext_payloads_sha256": plaintext_payloads_sha.hexdigest(),
    }
    if corpus_sha is not None:
        result["corpus_manifest_sha256"] = corpus_sha
    return result
