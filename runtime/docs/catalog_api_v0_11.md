# Persistent catalog API — Suffice 0.11

This document specifies the native in-process registry in
`include/suffice/catalog.hpp` and `apps/catalog/catalog.cpp`. It builds on the
0.10 exact payload pipeline. The catalog manages admitted immutable owners,
publication revisions, prepared queries, and complete-result retention across
multiple projects. It is a synchronous component; file capture, generation
loading, command transport, and crash-safe persistent publication are separate
boundaries.

## 1. Ownership and publication

`Catalog::publish(project, expected_revision, generation_label, snapshot,
artifacts)` accepts a fully validated `payload::Snapshot` and an owned vector of
`ArtifactRow { path, sha256, size }`. The generation label is descriptive; the
catalog assigns a monotonically increasing revision to each successful
publication or drop. A label can be reused, while its revision and immutable
generation owner remain distinct.

`state(project)` returns the currently observed revision and whether a snapshot
is loaded. Revision zero means that the project name has never occupied a slot.
`publish` compares the supplied expected revision against that state before any
change. A mismatch rejects a delayed writer. `drop(project, expected_revision)`
releases the registry's current owner and retains a tombstone with a new revision.
An old expected revision therefore cannot succeed after a present/absent/present
cycle. Project slots, including tombstones, are bounded. A catalog intentionally
has no unversioned forget operation that would reopen an absent-state ABA hole.

Publication validates:

- A nonnull snapshot with `all_payloads_verified == true`.
- Bounded nonempty project and generation labels without NUL.
- One metadata row per snapshot row and exact decoded-size agreement.
- Strictly sorted, unique relative slash-separated artifact paths. Absolute,
  drive/colon, backslash, empty-component, `.` and `..` paths reject.
- A bounded path length and a lowercase 64-hex digest field.
- Global logical snapshot, generation metadata, and live-owner count limits.

The programmatic API checks metadata shape and agreement of lengths. It does not
independently compute SHA-256, validate UTF-8, or prove that features were derived
from the bodies as intended. The generation loader establishes those stronger
artifact-to-body/extractor associations. Direct API callers carry the equivalent
responsibility. A syntactically valid digest is not by itself verification.

All allocating preparation completes before publishing the new project state.
Existing plans and result entries for that project are invalidated after the
nonthrowing state transition. A failed publication leaves the current generation
and existing cached results intact. The tests inject C++ allocation failures at
each encountered publication allocation point, for both new-project and
replacement cases.

Admission conservatively requires room for both old and new admitted owners.
The old current snapshot is counted until replacement succeeds. The publisher
does not evict a working generation to make an uncertain replacement fit. A
deployment should provide replacement headroom or explicitly drop an old
generation before loading another. This is a retained-owner admission rule,
not a claim to bound all candidate construction memory.

## 2. Immutable leases and retired generations

`acquire(project)` returns a copyable `Lease`. A lease exposes the project,
generation label, revision, dimensions and read-only artifact rows. It keeps its
own immutable generation alive but does not expose a raw `Snapshot` or `Query`
owner. Borrowed artifact spans/string views require the lease owner to remain
alive.

Publication does not mutate existing leases. A lease for the previous generation
continues to evaluate the old snapshot and emit its old complete bytes after a
new generation becomes current or after the project is dropped. Generation
identity—not merely a textual label, row count or hash—separates cache entries.
Leases from another Catalog instance reject.

Automatic plan/result retention is restricted to the current generation.
Executing an old lease prepares an ephemeral worker and returns
`retired_generation == true`; Populate does not add a retained result. This
prevents an old reader from silently creating hidden cache owners that keep a
retired generation alive after the reader releases its lease. Publication/drop
already invalidates retained entries for the project.

Snapshot accounting tracks weak references to every admitted snapshot. The same
snapshot owner reused in several generations is charged once. A direct caller's
external `shared_ptr<const Snapshot>` alias keeps an admitted snapshot charged
even after every catalog generation has released it. Generation metadata is
tracked separately through weak generation owners. `collect()` removes expired
accounting records; it does not invalidate a live lease. Statistics distinguish
current and retired retained bytes. Retired snapshot bytes can include an
external snapshot alias with no remaining generation metadata.

## 3. Execution, exact keys and output

`run(lease, source, first, count, sink, options)` evaluates the exact 0.10 result:
increasing global row IDs and every decoded byte for the selected range. The
source is bounded, nonempty, and NUL-free. A retained query key includes all
source bytes, including whitespace, and the immutable generation owner. The
result-cache key adds the exact global `first,count` range. Textually similar
programs do not share a result accidentally; there is no fingerprint-only key.

The plan LRU retains a compiled query and a reusable Worker. Repeated exact
queries against the current generation can reuse their preparation and decoder
storage. There is one mutable worker per retained plan and one serialized
operation at a time. Retired/uncached plans can execute transiently.

`RunOptions.backend` applies to computed misses and bypass runs. A complete
result-cache hit is independent of the requested compute backend and replays the
original cached Query owner; a forced backend that would be unsupported for a
miss may therefore still consume a hit. Backend preference is not part of the
observable result. `pipeline.backend` on a hit identifies the original cached
computation, while `cache_hit` identifies the actual replay path.

Output caps apply equally to hits and computed paths. Before any callback, a hit
compares its exact stored row count and output-byte count to `OutputLimits`.
Computed paths use the underlying exact selection preflight. Empty selected
bodies still consume a row allowance. If a callback throws, preceding rows may
already have been delivered; complete cached entries remain valid. A failing
consumer does not publish a new partially populated cache.

The runtime holds an independent generation owner while calling the sink. A
callback can reset the caller's separate Lease object without changing the
ongoing operation's lifetime. It cannot reenter a registry operation.

## 4. Explicit cache policies and costs

| Policy | Hit | Miss |
|---|---|---|
| Bypass | Lookup skipped | Compute full exact output; eligible plan reuse still applies. |
| Use | Replay every selected output byte | Compute full exact output; do not retain a new result. |
| Populate | Replay every selected output byte | Compute/output first; then optionally construct and retain a result. |

Populate charges the current implementation's real cost. It first completes a
normal output-producing execution. On a successful current-generation miss, a
bounded result is built using the inherited two-pass `ResultCache::build`:
count/decode, allocate exact retained storage, decode/fill. These are two
additional passes after the first output. `cache_build_ms` reports the optional
construction and admission phase separately from `execution_ms`; full command
or session timing must include both. A cache hit still sends all plaintext to
the same sink.

An inexpensive selected-size estimate skips obviously oversized results. The
authoritative retained-capacity check remains `ResultCache::build` and
`storage_bytes()`. The global byte limit applies across projects. An entry-count
limit also bounds empty results and per-entry key/query objects. LRU recency is
updated on successful-cap preflight hits. A candidate can displace multiple
victims until both byte and count limits fit. Candidate construction and its list
node allocation finish before existing entries are removed; a failure cannot
leave a partially published result.

Optional cache-construction exceptions do not undo already completed output.
They return `cache_build_failed=true` and no new cache entry; oversized results
return `cache_oversized=true`. A consumer failure during the first output phase
propagates and does not start cache construction. This is explicit retention
policy, not an adaptive claim that every admitted cache will repay its cost.

For `K` identical uses under Populate-on-first-use, the modeled session cost is

`T_direct + C_build + (K-1) * T_cache`,

versus `K * T_direct` without results retained. With positive per-replay savings,
the conditional total-use threshold is

`K >= 1 + ceil(C_build / (T_direct - T_cache))`.

Use complete measured sessions when choosing a deployment policy. Invalidation,
eviction, output cost, cold admission, cache competition and memory pressure can
change the benefit. An earlier build-before-replay model has a different first
use and cannot be copied as this policy's threshold.

## 5. Resource accounting

| Limit/statistic | What it covers | Additional storage outside that number |
|---|---|---|
| `max_snapshot_bytes` / `snapshot_bytes` | Logical raw features, packed features, payload store and derived block ranges for all live admitted Snapshot owners, deduplicated by owner. | Candidate read/copy buffers, object/allocator overhead, decoded worker rows. |
| `max_metadata_bytes` / `metadata_bytes` | Generation object size, retained artifact-vector capacity, and project/generation/path/digest string capacities including terminators. | Project map/tombstone nodes and allocator bookkeeping; these have independent count/label limits. Small-string capacities can be conservatively counted again inside object storage. |
| `max_result_bytes` / `result_bytes` | Retained complete-result row-vector and byte-vector capacities across all projects. | Result nodes, exact source keys, retained Query owners, construction scratch. Entry and program-size limits bound their counts/key sizes. |
| `max_plan_text_bytes` / `plan_text_bytes` | Cached plan source-string capacities including terminators. | Compiled plan structures, worker buffers, list nodes. Plan count is bounded separately. |
| `plan_worker_bytes` | Sum of current `Worker::scratch_bytes()` over retained plans. | Transient workers, decoder/library allocation overhead and other process allocations. |
| Live snapshot/generation and project slot limits | Number of tracked live input owners, metadata owners, and named/tombstoned projects. | A direct caller can construct unrelated inputs outside the catalog. |

Result source keys can share their string allocation with a retained plan. The
separate key and plan statistics count their specified ownership categories and
must not be summed as a deduplicated process-memory estimate. None of these
logical/component numbers is RSS, a global allocation-free guarantee, or a cap
on total process memory. Changing a default byte limit does not bound external
consumer behavior.

`cache_hits`/`cache_misses` count lookups, including a matching hit subsequently
rejected by output limits or a consumer. `runs` counts completed output
operations. `plan_preparations` counts successfully constructed plan/worker
objects. Evictions and project invalidations are reported separately.
On a cache hit, feature requests, compressed reads, decoding and block-decision
work are zero; selected rows and logical output bytes remain the complete result.

## 6. Concurrency and verification boundaries

Calls that inspect or mutate registry state require caller serialization. The
native CLI provides this by processing one request at a time. The operation
guard detects same-Catalog reentrancy and resets during exception unwinding; it
is not a mutex and does not make unsynchronized concurrent calls safe. The
immutable `limits()` accessor is exempt from the reentrancy restriction. Separate
Lease values may cross threads, but executing them through this Catalog still
requires external exclusion.

The focused native suite checks two-project output, replacement/retired leases,
stale CAS and tombstones, external snapshot aliases, shared-owner accounting,
metadata/owner limits, global variable-size LRU eviction, empty-result entry
limits, exact source/range keys, bounded plan retention, output caps, sink
failures, reentrancy, metadata rejection, and allocation-failure publication
atomicity. Comparisons use original fixture bytes. Injected C++ `new` failures
do not exhaustively inject failures inside the codec's C allocator or operating
system. Native Windows and multithreaded deployment require their own platform
validation; the portable source alone does not establish those results.
