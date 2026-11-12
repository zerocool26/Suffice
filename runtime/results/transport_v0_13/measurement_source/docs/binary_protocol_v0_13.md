# Bounded binary responses — Suffice 0.13

## Purpose and compatibility

The native catalog now offers `--transport jsonl|binary`. JSONL remains the default. Both transports return the same ordered generation-local row IDs, exact UTF-8 path bytes and complete original bodies. Query selection, generation identity, range semantics, result/worker retention and diagnostic FNV calculation are unchanged by transport selection.

Requests retain the existing bounded TAB-separated ASCII format and terminating LF. Text request fields remain hexadecimal UTF-8; this release changes responses, not command encoding. `RUN` and `RUNPIN` still accept `hex` for complete output and `hash` for diagnostic consumption. The historical name `hex` denotes complete-output mode even when the chosen binary transport sends raw bytes. Hash mode emits no row events and does not return artifact contents.

For example, a binary child is started as:

```text
suffice_catalog serve --transport binary
```

There is no unsolicited greeting or format negotiation. The parent selects the transport when it starts the child. Binary receivers validate the versioned magic on their first response and every subsequent frame. A mismatch must not trigger an attempt to reinterpret partially consumed data as another transport.

Each fully handled request has zero or more row events followed by exactly one terminal result. Command failures can report a completed prefix. Stream failure may instead terminate the process without a terminal result; it cannot be recovered by resynchronizing the same connection.

## Binary frame header

Every binary event starts with this exact sixteen-byte header. All integer fields are unsigned and little-endian. The writer encodes bytes explicitly; native structure size, alignment and endianness do not define the wire format.

| Byte offset | Size | Field | Required value |
|---:|---:|---|---|
| 0 | 4 | Magic/version | ASCII `SFC1` |
| 4 | 1 | Kind | `1` for terminal JSON; `2` for a row |
| 5 | 1 | Flags | Zero |
| 6 | 2 | Reserved | Zero |
| 8 | 8 | Payload length | Bytes following the header; at most 4,194,304 |

Unknown magic/version, kind, flags or reserved bits are fatal protocol errors. Zero-length terminal payloads and row payloads shorter than their required metadata are invalid. The payload-length cap must be checked before allocating or reading a buffer of the advertised size.

The version and reserved fields leave room for an explicitly defined future protocol revision. Receivers must not assume that an unknown flag or kind is harmless padding.

## Row payload

A kind-2 payload begins with four uint64 fields and then two byte strings:

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Request sequence |
| 8 | 8 | Global row ID within this generation |
| 16 | 8 | Path length in bytes |
| 24 | 8 | Body length in bytes |
| 32 | Path length | Exact UTF-8 path bytes |
| 32 + path length | Body length | Exact original body bytes |

The frame must satisfy:

\[
32+\text{path length}+\text{body length}=\text{payload length}.
\]

Path length is between one and 32,768 bytes. Paths must be valid Unicode-scalar UTF-8 without NUL. The native disk-admission path has the tighter default path limit of 4,096 bytes; the framing limit does not relax admission. Body bytes have no text interpretation: NUL, CR, LF, Ctrl-Z and every other byte value are valid body data. An empty body is valid and still carries its row and path.

The writer first bounds the path, then checks body length against remaining payload headroom. It does not form an unchecked sum of arbitrary lengths. The receiver similarly validates the fixed row prefix and exact nested extents before accepting the variable bytes. In addition to per-frame limits, receivers must enforce cumulative request row/body limits and the requested row range. Individually legal frames do not establish a bounded total response.

A row is delivered to the consumer only after its entire frame is available and validated. A short body or path is not a smaller successful row. Row IDs remain generation-local; binary framing does not make them stable identifiers across edits.

## Terminal JSON payload

A kind-1 payload contains one complete UTF-8 JSON result object, without an LF terminator. Its shape is the existing terminal object: `type`, `seq`, `status`, operation-specific fields and diagnostics. Successful full-output `RUN`/`RUNPIN` results include `selected_rows`, `output_bytes` and the ordered diagnostic fingerprint. Error results include `error`, `delivered_rows` and `delivered_bytes`.

The client must validate the terminal object's type, sequence, status and required counters. For complete output, success counts must agree with accepted rows and body bytes. Error prefix counts must agree with the completed prefix. Hash mode has no row frames: its terminal counters instead describe the rows and bytes consumed into the diagnostic hash. FNV remains a diagnostic checksum, not cryptographic authentication.

The native application assembles terminal JSON through the same local stream path for both transports. That local stream enables exceptions on formatting/allocation failure so a partially formatted string cannot be emitted as a successful result. The response writer checks the final wire extent before emitting its header or bytes. Terminal staging is distinct from a hard whole-process allocation budget; the protocol cap bounds emitted payload size.

## JSONL boundary

JSONL rows retain `path_hex` and `data_hex` and a terminating LF. The native writer now checks each JSON row's full encoded size before writing it, using exact decimal field lengths and doubled path/body lengths. Its maximum is 4,194,304 bytes including LF. Terminal JSONL has the same line cap, so its JSON payload maximum is one byte smaller.

The different encodings therefore have different maximum representable body sizes under the same numerical wire cap. The native loader's default one-MiB body admission limit fits both. Comparisons must use data and requests supported by both formats; a larger binary-only frame is not evidence of a faster equivalent JSONL request.

All native terminal metadata, FNV work and logical output semantics are held constant between the two response transports. JSONL and binary are selectable configurations of the same 0.13 executable. This does not establish equality with historical 0.12 timing or its exact formatting implementation.

## Failure and lifecycle rules

| Event | Native action | Connection status |
|---|---|---|
| Invalid command or logical output limit | Emit a terminal error with its completed prefix | Usable after the complete error is consumed |
| Per-row framing rejection before any bytes of that row | Emit a terminal error covering earlier complete rows | Usable after the complete error is consumed |
| Decode or sink exception with output still sound | Destroy any private cache candidate and report the completed prefix | Usable only if the complete terminal error is received |
| Short/failed output write, thrown stream-buffer write or failed flush | Poison the writer and terminate; do not append an error frame | Unusable |
| Truncated, malformed or inconsistent response at the receiver | Reject the response and terminate the owned child | Unusable |
| Consumer callback raises | Terminate the owned child so pending rows cannot become another request's response | Unusable |

Poisoning is sticky: clearing the underlying C++ stream state cannot re-enable the writer. Partial binary headers and partial payloads cannot safely be followed by an ordinary error event. The dedicated wire tests inject failures at header, prefix, path/body and flush boundaries, with both default and throwing ostream error masks.

The runtime is caller-serialized. A callback must not issue a nested request on the same client. A normal terminal native error and a callback-raised error of the same Python exception class have different synchronization consequences; only the former can preserve the connection.

Completed-prefix counters are set after a successful runtime call as well as on an exception from that call. This preserves their meaning if terminal JSON assembly fails after rows have already been written. A stream failure is fatal even if rows were previously accepted into an output buffer; there is no delivery acknowledgement or external sink transaction.

The native sequence counter refuses to wrap uint64. A request after its representable sequence space is exhausted terminates the service rather than reusing sequence zero.

## Windows byte preservation

Before server I/O, the Windows build sets both stdin and stdout CRT descriptors to `_O_BINARY` and checks both calls. This applies to both serve transports so the protocol uses exact LF and binary bodies do not undergo CR/LF or Ctrl-Z processing. Help/version output is a separate text CLI path.

The implementation is provided for native Windows builds; Linux tests do not validate the Windows CRT path. Any platform-specific claim must cite an executed Windows build and protocol receipt.

## Cost and measurement contract

For a binary row, total response bytes are:

\[
16+32+\text{path bytes}+\text{body bytes}.
\]

JSONL adds textual framing, decimal integers and twice the path/body byte count. For large bodies, removing hexadecimal representation approaches a halving of those represented bytes. It does not imply a proportional service-time improvement: decoding, FNV, admission, copying, callbacks, terminal metadata and scheduling remain.

Binary control responses add a sixteen-byte frame header where JSONL uses a one-byte terminator. A control-only or tiny-output workload can pay more wire bytes or parsing work. Preserve those cases in measurement.

Count all response bytes read from the pipe, including every frame header, row prefix, path, body and terminal payload. Count request bytes separately. Keep wire volume, logical returned body bytes and source-corpus bytes distinct. Compare full consumer-visible outputs and complete service/raw wall time under the same admission, cache, worker and callback/collection conditions.

The native binary row writer uses a sixteen-byte header and thirty-two-byte row-prefix stack buffer, then writes existing path/body spans directly. This removes the native hexadecimal conversion path; it is not a zero-copy claim. Stream buffers, kernel pipes and the Python consumer still copy or materialize bytes.

## Implementation and evidence

- `apps/catalog/wire.hpp`: explicit serialization, preflighted wire extents and sticky output failure.
- `apps/catalog/main.cpp`: transport selection, unchanged commands, row sink, terminal staging and platform setup.
- `tests/catalog_wire_main.cpp`: golden bytes, integer/path/body bounds, empty bodies, UTF-8 preservation and injected write/flush failure.
- `scripts/catalog_client.py`: selected response decoder and the unchanged consumer row shape.

The standalone wire gate passed nine groups before full release validation. Complete build, client/integration and measurement outcomes belong in their executed receipts and release report. This document defines the format and implementation contract; it does not substitute for those results.
