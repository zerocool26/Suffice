# Suffice 0.12 engineering audit

Status: read-only source review of the preserved 0.11 implementation. No runtime, experiment or competition manuscript was modified by this review.

## Strongest concrete opportunities

### 1. Prepared-query retention is coupled to mutable execution storage

`Catalog::Impl::Plan` owns the generation, exact source, immutable prepared Query and a Worker. `plan()` constructs every component before admission and retains the whole bundle under entry and source-text limits. `Stats::plan_worker_bytes` reports worker component capacity, but no worker byte limit is enforced. A count cap therefore permits a small number of disproportionately large workers.

The Worker constructor eagerly creates a Decoder, a whole-snapshot selection mask, a block mask, and every available Wordcode and Dense executor. The Decoder allocates a buffer as large as the snapshot's maximum decoded row and a Zstandard context. Default Auto maps to Zoned; Dense storage is still allocated despite not being used by that backend. All-false selections can retain a decoder that has never emitted a byte.

Source locations: `apps/catalog/catalog.cpp` (`Impl::Plan`, `Impl::plan`, `Impl::stats`); `apps/payload/payload.cpp` (`Worker::Worker`, `Decoder::Decoder`, `Worker::scratch_bytes`); `include/suffice/catalog.hpp` (`Limits`, `Stats`).

#### Minimal next architecture

Keep the immutable Query in the existing Plan, but make its Worker optional and independently evictable. Reuse the existing miss-access plan ordering for a first worker-LRU policy. On a retained-plan hit with no worker, create a Worker from the existing Query instead of reparsing/recompiling. Evict only Worker ownership when enforcing the worker byte or entry budget; keep inexpensive query metadata available.

Distinguish query reuse from worker reuse in telemetry. Report worker creation, reuse, eviction and oversized-transient execution separately. These are resource-policy facts, not a claim of a new cache-replacement algorithm.

#### Correctness obligations

1. Executors borrow immutable Query/Snapshot structures. A Worker must retain that exact Query owner; blind rebinding is unsafe.
2. Zstandard context size can grow during decode. Admission-time charges are insufficient. Reconcile actual component capacity at the end of each call.
3. Decode or sink exceptions can occur after growth. Cleanup must enforce the retained budget on exceptional exits too, without allocating or throwing a second exception.
4. A worker larger than the retained budget can execute transiently, then be released. This policy bounds retained component storage at API quiescence, not peak construction memory or process RSS.
5. Cache replay must not acquire a worker. A result already owns complete output and its exact Query identity.
6. Retired-generation calls retain neither new plans nor workers. Clearing or invalidating plans releases their optional workers.
7. Use checked/subtractive budget arithmetic. Avoid calculating `current + candidate <= cap` with an unchecked sum.

Existing regression anchors: sink failures/reentrancy, old-generation leases, allocation-failure publication preservation, zero retention, plan eviction with independent result ownership.

### 2. Complete-result construction decodes data merely to count it

`Catalog::run(Populate)` first performs and delivers a normal Worker run. `ResultCache::build` then creates another Worker, decodes all selected bodies once into a counting sink, allocates exact row/byte vectors, and decodes all bodies again into a writer. One successful Populate miss therefore traverses selected decoded payload three times.

The counting decode is avoidable: Snapshot already stores checked decoded row lengths, and Worker already creates an exact selection mask before any output. `Snapshot::Snapshot` checks each frame's declared content size against the indexed decoded size; successful full admission has also decoded the bodies. Selecting a mask and accumulating lengths requires no body decompression.

Source locations: `apps/catalog/catalog.cpp` (`Catalog::run`); `apps/payload/payload.cpp` (`Snapshot::Snapshot`, `Worker::run`, `ResultCache::build`); `include/suffice/payload.hpp` (Worker, ResultCache and RowSink contracts).

#### Safe progression

First separate selection/preflight from emission internally. Count exact selected rows and decoded bytes from the mask and immutable index. This enables a one-decode ResultCache build while preserving the public identity contract. A stronger integration allocates a private candidate after preflight and captures the same decoded bytes during normal delivery, then publishes only after every callback succeeds.

For fused capture, all of the following are required:

- Finite output limits reject before any sink callback, as they do today.
- Optional cache allocation or capacity failure disables retention while allowing the requested normal output to succeed.
- Allocate and validate the candidate before evicting any existing result.
- A throwing sink or decoder publishes no candidate. Preceding rows may already have been delivered under the existing streaming contract.
- Commit only complete row IDs, offsets, lengths and copied bodies, with the same generation/source/range/Query identities.
- Zero selected rows and selected empty bodies remain distinct; empty bodies still consume row budget and entry metadata.
- Counters and timers must reflect the changed boundary. Fused copying belongs inside execution; a smaller separately reported cache-build timer alone is not evidence of a faster whole request.

No public API should accept an arbitrary caller-supplied mask or unverified shape as proof of output identity. The runtime must generate and retain the selection under the matching immutable query owner.

## Established boundaries that should remain intact

- Direct `Catalog::publish` trusts caller-provided external semantics; the disk loader independently verifies them.
- Caller serialization and the reentrancy guard protect same-Catalog mutable state; this is not concurrent-reader execution.
- Snapshot/generation weak-owner charges account for retained aliases and old leases.
- Plans/results are staged before existing entries are evicted, preserving prior state on allocation failures.
- The full generation loader currently decodes twice for distinct checks. Eliminating that duplicate admission work needs a separate verified-interface change and should not be silently mixed with query/caching changes.
- A worker component budget is not a bound on compiled Query structures, result storage, allocator overhead, consumer buffers, OS caches or peak RSS.

## Recommended focused validation

Worker retention: exact byte cap and zero cap; byte oversize with successful output; entry cap; several unequal workers requiring multiple evictions; plan reuse after worker eviction; true worker reuse; backend-driven growth; sink/decode failure after growth; cache hits requiring no worker; retired-generation output with no retained worker.

Fused cache capture: complete bytes against an independent oracle for every backend and range; sparse/zero/empty-body selections; row/byte output limits before callbacks; candidate allocation failure; sink failure at first/middle/final callback; existing cache survival; exact source/range/generation identity; entry and byte capacity bounds; both full and on-demand Snapshot semantics if the public payload API supports both.

Performance: compare complete service cost, construction work and retained worker components with paired policy order. Retain a legacy configuration, a strict worker budget, and a scan-pressure workload. Report cold/warm transitions and policy-induced recreations. Do not infer whole-process memory reductions from component sums alone.
