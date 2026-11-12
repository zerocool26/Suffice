# Independent source review — Suffice 0.13 response transport

## Scope and disposition

This is an independent AI-assisted, read-only review of the native response writer, its integration into the persistent server, the Python response decoder, the wire specification, and focused test sources. No runtime files were edited. No native build, benchmark or test suite was executed by this review.

**Final disposition: no unresolved blocking framing, extent, lifetime, synchronization or wire-accounting defect found in the inspected paths.** One narrow hash-mode terminal-conformance gap was sent to the author and root coordinator, corrected, and checked again against the final source. Its resolution is recorded below. This review is not a formal proof, a substitute for executed tests, or a Windows validation result.

## Inspected identities

Paths are relative to `suffice_v0_13/`.

| File | SHA-256 |
|---|---|
| `apps/catalog/main.cpp` | `c80c80e3550a9da03f0fa4c54f7872c4f69c0a494eabbe90a87628c56372d5b5` |
| `apps/catalog/wire.hpp` | `064d9a4f9e9bb993d8f1afdbf592e033af7f883046c6b631e52abe714b53002d` |
| `scripts/catalog_client.py` | `f0282195bf939aded456aa54258bcf0c237ccd9255660d665875feaaf6680495` |
| `tests/catalog_wire_main.cpp` | `5cdc7ea23404b8ef5f0214be89a6bc0651d3497992c64575eb07563708565fe1` |
| `tests/test_catalog_framing.py` | `ce3c84f44353420fc727591526a339d3f17d9594aff07727842dafb5e6d5512c` |
| `docs/binary_protocol_v0_13.md` | `f4e1488a5ad5faefa4d1127765274ad3a868be49238106d2e46d8baf6aad863c` |

These identities define the final reviewed snapshot and were independently recomputed after the narrow correction. Any subsequent source changes need a narrow follow-up review or a clearly recorded difference.

## 1. Format agreement and bounds

Both implementations agree on a sixteen-byte `<4sBBHQ` header: magic `SFC1`, kind 1 for terminal JSON or 2 for a row, zero flags/reserved, and a little-endian uint64 payload size. Both enforce the 4 MiB payload cap. A binary row carries four uint64 values—sequence, row ID, path byte count and body byte count—before its path and body bytes. Path bounds are nonempty through 32,768 bytes, strict UTF-8, without NUL.

The native writer bounds each variable component before addition. Its JSONL row bound accounts for exact decimal digits, fixed syntax, doubled path/body bytes and LF. The small fixed syntax cannot approach the subtraction bound. The `std::streamsize` static assertion covers the maximum write size. Wire fields are encoded byte by byte rather than by copying native struct layout.

The Python reader checks header kind/version/reserved bits and payload cap before variable payload reads. It validates the complete row-prefix equation and remaining body budget before requesting path/body extents. Python integer arithmetic cannot wrap; subsequent read sizes are bounded by the validated frame. Short reads loop until the requested extent is complete or EOF makes the response fatal.

The JSONL decoder independently checks its line cap, strict UTF-8 JSON, duplicate object keys, nonstandard nonfinite constants, required uint64 fields, and exact hex extents. Whitespace accepted by Python's `bytes.fromhex` is rejected by a postconversion extent check. Booleans are not accepted as protocol integer fields.

The wire cap is intentionally a different body-size ceiling for JSONL and binary. The documented default native one-MiB body limit fits both. A performance comparison must stay within the shared supported domain.

## 2. Partial writes and terminal construction

The native writer validates row and terminal extents before emitting their bytes. Its binary row path writes the existing body span with small stack headers, rather than allocating a body-sized encoded copy. This does not eliminate stream/kernel/client copies.

All actual output occurs inside the writer's guarded emission path. A short/failed write, throwing stream buffer or failed flush makes poisoning sticky. Clearing the underlying ostream does not restore the writer. The server returns on poisoned output and does not append a normal error result after a partial frame.

Terminal JSON is staged through an `ostringstream` with badbit/failbit exceptions enabled. A partial formatted object cannot silently pass through as a successful JSON terminal. Its finished extent is checked before output. Failure while building error metadata can terminate the service; the client handles the resulting incomplete transaction as fatal rather than reusing the connection.

RUN prefix counters are copied from the sink on exceptional exit and immediately after successful execution, before terminal formatting. A later terminal-format failure can therefore report the earlier complete prefix correctly when output is still sound. The sink increments delivered rows/body bytes only after its row writer succeeds. A framing preflight rejection leaves the current row uncounted and can still produce a recoverable error for preceding complete rows.

## 3. Receiver synchronization and callbacks

The client permits row events only for a valid full-output RUN/RUNPIN envelope. It enforces strictly increasing row IDs, the requested start/count range, cumulative row/body limits and success/error terminal count equality. In `collect=False` mode, separate observed counters preserve these checks without relying on the collected list.

Successful terminals must identify the active sequence and requested operation. Error terminals require valid status, error text and delivered-prefix fields. An unknown type/status, malformed JSON, truncation, sequence mismatch or inconsistent full-output accounting closes the owned child. Only a fully validated native error sets the recoverable-terminal flag.

A callback-raised `CatalogError` therefore remains distinct from a parsed native error of the same class. Same-client nested requests are rejected before sequence mutation or a write. Graceful close records its own QUIT result; a failure during graceful close falls back to forced child cleanup.

The prior stdout-EOF path's blocking stderr read has been removed. EOF now produces a fatal protocol error and terminates the child without waiting for diagnostic EOF. The focused test source contains real child probes that keep stderr/process alive after closing stdout; their executed outcomes belong in the validation receipts.

## 4. Wire telemetry

Per-request counters distinguish request bytes, row-response bytes, terminal/control-response bytes and their sum. Cumulative counters remain on the client after close and include the implicit QUIT exchange. The terminal object does not attempt to describe its own encoded length; the reader measures actual consumed event lengths.

These are application-stream bytes accepted/consumed by successful operations, not packet counts, kernel copies or physical I/O. Python buffering can prefetch bytes internally, and a failed flush does not certify that every buffered request byte reached the child. The planned successful-session experiment can use the counters as wire-representation accounting. Failed sessions need their own incomplete-transfer interpretation.

## 5. Resolved finding: explicit range in hash-mode terminals

In the initial inspected client, hash-mode success and error terminals are checked against `max_rows` and `max_bytes`, but not against an explicit RUN `count`. A forged terminal can therefore report one selected row for `count=0` while remaining below `max_rows`, and still be treated as a synchronized terminal. Full-output mode rejects the analogous inconsistency through its actual ordered row IDs.

This does not affect correct native output or the proposed full-output timing experiment. It is a small conformance gap in malformed-peer handling, not a memory-bound escape.

**Resolution: corrected and independently source-checked.** The final client rejects successful selected-row counts above an explicit RUN count, and applies the same bound to delivered-row counts in a hash-mode error prefix. Existing `max_rows` and `max_bytes` checks remain in place. The final test source covers forged success and error terminals at explicit counts 0 and 1 in both transports, plus valid count-1 completions. Those malformed responses go through the existing fatal protocol-error path, so the connection is not reused after accepting contradictory range metadata.

The initial client identity was `6c875bfbfddbdd844df242e7f2f59b29b483d59b74f4fcdd675a449847987909`; the final client and test identities appear in the table above. The author reported 26 focused tests passing after this correction. This reviewer checked the source and hashes but did not rerun the suite; executed evidence should be taken from the release validation receipts.

## 6. Explicit limits of the review

- No request deadline is introduced by finite frame lengths. A live child can stop partway through a legal frame; a separate tested timeout/cancellation mechanism would be needed for a wall-time guarantee.
- Stderr is still a separate undrained pipe during normal requests. The controlled native service's bounded diagnostic behavior remains relevant; fixing EOF cleanup alone does not solve arbitrary stderr flooding by another child program.
- The protocol validates the response envelope and required delivery fields. It does not authenticate an arbitrary child or independently prove all operation-specific statistics and body contents; the experiment's independent original-body oracle supplies the latter check.
- Native and client buffers, collected rows, callbacks and Python objects remain outside the retained-Worker budget. The frame cap is not an RSS bound.
- Windows `_O_BINARY` setup appears in both serve modes before native input/output and checks failure. Its actual behavior requires native Windows execution; Linux source review cannot supply that evidence.
- The same 0.13 terminal staging/validation path is used for both transports. This supports a within-version comparison and does not make its JSONL timing identical to historical 0.12 behavior.

## Test-source assessment

The native fixture spells out golden little-endian bytes independently, covers exact and one-over caps, empty bodies, UTF-8 paths, maximum integers and sticky failures under both normal and throwing stream masks. The Python fixture constructs wire bytes independently of the client struct parser, covers every split/truncation position for a small representative response, cumulative limits, malformed terminals, callback failures, reentrancy, QUIT accounting and real EOF/open-stderr cleanup.

This is meaningful boundary coverage rather than a second implementation used as its own oracle. Test counts and pass/fail claims should be taken from final receipts whose source hashes match the packaged files. This reviewer did not repeat those tests or the native benchmark.
