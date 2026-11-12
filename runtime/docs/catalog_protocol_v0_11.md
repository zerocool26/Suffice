# Persistent catalog protocol — Suffice 0.11

`suffice_catalog serve` is a persistent child process over stdin/stdout. It opens
no network socket and installs no background service. The Python client in
`scripts/catalog_client.py` keeps that process alive across projects and queries.
The registry is serialized; use one client request at a time. A row callback may
consume or copy the row but must not issue a nested request on the same client.

## Wire format

Each request is an ASCII line with TAB-separated fields and a terminating LF.
Text is encoded as lowercase hexadecimal UTF-8. Integers are unsigned decimal.
The native reader retains at most 256 KiB of a request line and discards the
remainder of an overlong line before reporting an error. Text fields reject NUL;
program source is limited to 64 KiB and labels to 256 bytes by default.

| Operation | Fields following operation |
|---|---|
| `OPEN` | project hex, catalog-root hex, expected current in-memory revision |
| `STATE` | project hex |
| `PIN` | new alias hex, current project hex |
| `UNPIN` | alias hex |
| `RUN` | project hex, program hex, first row, count or `all`, cache mode, output mode, max rows, max bytes |
| `RUNPIN` | pinned alias hex, then the same fields as `RUN` |
| `DROP` | project hex, expected current in-memory revision |
| `STATS` | no fields |
| `CLEAR` | `results`, `plans`, or `all` |
| `QUIT` | no fields |

Cache mode is `bypass`, `use`, or `populate`. Output mode is `hex` or `hash`.
`OPEN` reads disk CURRENT once, validates that immutable generation, then uses
the in-memory expected revision to publish. Disk generation IDs and in-memory
revisions have different scopes: a new server session begins with no projects;
revisions are global monotonic counters within that Catalog instance.

## Responses and output

Responses are JSON lines with a monotonically increasing request sequence. A
request has zero or more `type: row` events and exactly one terminal `type: result`
event. Hex row events contain the global row ID, path encoded as UTF-8 hex, and
every decoded payload byte encoded as hex. A row ID belongs to one generation;
it must not be used as a stable artifact ID across generations.

The client API decodes rows to `{row, path, data}` with `data` as Python bytes. It
can collect rows, stream them to a callback, or do both. The native output caps
bound logical selected rows and bytes, not doubled hex length, JSON overhead,
Python objects, or a callback's allocations. Hash mode still reads every byte
into an ordered diagnostic FNV hash; it returns no artifact bodies and does not
provide cryptographic authentication. Full-byte comparisons use hex mode.

Limits are preflighted before a row callback, including cache hits. Other failures
can leave a delivered prefix. Error results report completed row/byte delivery.
If a client callback raises, the client terminates its owned child process so a
later request cannot consume the previous request's remaining response. A normal
native terminal error is fully consumed and the connection remains usable.

The client distinguishes a callback-raised `CatalogError` from an error parsed
from a terminal native response. The former closes the connection. Nested client
requests reject before mutating the sequence or writing to the child.

## Admission and retention scopes

Default native limits are 256 MiB logical admitted snapshot bytes, 32 MiB retained
artifact metadata, 64 MiB result-vector capacities, 32 project-name slots, 32
prepared plans, and 64 result entries. Plan-source capacity has a separate
512 KiB limit. Result key storage, prepared worker storage, construction copies,
allocator overhead, consumers and process infrastructure are distinct. Stats
expose result key bytes and current retained worker storage; none is a peak-RSS
measurement.

The CLI preflights new generation inputs against remaining snapshot/metadata
headroom before retaining the candidate. Replacement conservatively requires
both old and new owners to fit. This preserves the old state if admission fails.
Old leases and caller-held Snapshot aliases remain charged. Queries on a retired
lease use ephemeral preparation and do not repopulate the internal caches.

`DROP` removes current ownership and invalidates that project's automatic reuse.
It preserves a bounded project-name tombstone, so stale revision zero cannot
silently republish a previously used name. `CLEAR` releases selected reusable
state but preserves current snapshots. Neither operation deletes disk generations.

## Telemetry

`OPEN.load_ms` includes captured-component reads/hashes, Full Snapshot validation,
and independent decoded-body SHA/feature verification. Full validation and the
semantic check each decode all rows; `verified_body_bytes` counts the second
pass only. It must not be labeled total decoder work or physical disk I/O.

Runs report preparation, execution and optional cache-build time separately.
`execution_ms` includes the consumer: for this CLI it includes full-byte FNV and
hex output when selected. `wall_ms` includes the entire native command through
the point it formats the terminal response. `client_wall_ms` includes the child
round trip and Python decoding/materialization until the terminal response is
parsed. These clocks overlap and must not be added together.

Populate first delivers an ordinary result, then optionally constructs a cache
in two additional passes. Cache build time and retained bytes are charged.
For K total identical requests, the conditional model is

`T_direct + C_build + (K-1)*T_replay <= K*T_direct`.

The conditional threshold is `1 + ceil(C_build/(T_direct-T_replay))` when the
denominator is positive. The release's complete-session experiment measures the
actual API and does not transfer the earlier build-then-replay threshold.
