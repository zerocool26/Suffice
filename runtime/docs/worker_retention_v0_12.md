# Independent worker retention and complete-output capture

## Change in 0.12

A prepared query and a mutable execution worker now have different retention lifetimes. Evicting a worker releases its decoder, masks and execution scratch while leaving the exact prepared query available. A later miss can construct a new worker without parsing or compiling that query again. Complete-result replay acquires no worker.

`PopulateStrategy::Capture` is the default population strategy. It creates a private bounded result candidate from the internally computed exact selection, captures bodies during the normal output stream, and stores the candidate only after successful delivery. `PopulateStrategy::Recompute` retains the 0.11 comparison path: one normal run followed by two additional body-decode passes for cache construction. Both retain the same generation/source/range keys and Query ownership checks. The older strategy is a controlled comparison, not the production default.

These mechanisms preserve the existing source and output identities. They do not claim a novel replacement algorithm, an improved language model, or a universal performance advantage.

## Limits and public interface

| Field | Default | Meaning |
|---|---:|---|
| `Limits::max_worker_bytes` | 16 MiB | Sum of retained workers' reported component capacities at API quiescence |
| `Limits::max_worker_entries` | 32 | Number of retained workers at API quiescence |
| `Limits::max_plan_entries` | 32 | Independent number of retained prepared-query plans |
| `Limits::max_plan_text_bytes` | 512 KiB | Independent retained source-text budget |

The native server accepts `--max-worker-bytes N`, `--max-worker-entries N`, and `--population capture|recompute`. Population is a server-wide default for RUN/RUNPIN without changing their tab-separated field format. The C++ API selects it per call with `RunOptions::population`.

`Catalog::clear_workers()` and protocol `CLEAR workers` release mutable workers without invalidating prepared queries or complete results. `clear_plans()` releases both queries and attached workers; results independently retain the Query owner needed for exact replay. Publication/drop still invalidate all affected plans and results. Old leases still execute without creating new retained plans or workers.

Zero worker bytes or zero worker entries disables worker retention while allowing ordinary execution. A worker whose component size exceeds the byte limit executes transiently and is released before the call returns. A disabled plan cache also prevents worker retention, because there is no retained parent plan.

## Quiescent invariant

Let `W` be the workers attached to retained plans after a public operation returns or throws, and let `b(w)` be the worker's current reported component capacity. The enforced invariant is:

```text
|W| <= max_worker_entries
sum(b(w) for w in W) <= max_worker_bytes
```

This is a retained component bound. Active worker construction can temporarily exceed the retained budget. Compiled Query structures, snapshot and generation storage, complete results, allocator bookkeeping, consumer allocations, OS caches and process peak RSS are separate. The worker remains eagerly prepared in this release; lazy backend/decoder construction is not implemented.

Every completed computation measures the worker again. This matters because codec contexts can grow while decoding. The same allocation-free scope cleanup runs when output limits, decoding, allocation, or a sink callback throw. An exception cannot leave a newly oversized retained worker behind merely because the successful-return code was skipped.

### Eviction order

The prepared-plan list is ordered by computed-miss access; a complete-result hit does not use or touch a worker. Worker retention uses that ordering without deleting prepared queries:

1. Release individually oversized workers first. Such a candidate does not evict other workers that could fit.
2. Walk remaining workers from newest to oldest, maintaining a remaining-byte balance and kept-entry count.
3. Retain the newest fitting prefix; after either cap prevents retaining a worker, release it and all older workers.

The prefix rule is equivalent to removing oldest workers until both constraints hold, after excluding individually oversized workers. It intentionally does not skip a large older worker and then retain even older small workers. A future cost-aware admission policy would be a separate change.

The budget arithmetic subtracts each accepted size from remaining headroom. It does not form an unchecked `used + candidate` sum. Release and list scans allocate no storage. Worker destructors release the decoder and executor allocations; no throwing report/string allocation is performed from scope cleanup.

## Ownership and failures

The Query and Snapshot remain immutable owners. Worker executors borrow structures belonging to that exact Query, so a worker is recreated from its Query rather than rebound to a different one. A local shared Plan owner remains alive through execution, even if the plan is not retained. The existing same-Catalog reentrancy guard continues to reject callbacks into mutable catalog operations.

Preparing a new query still stages its list node before evicting old plans. Worker construction may fail afterward; the successfully prepared query can remain useful, but no worker is retained from a failed constructor. This is a cache-warming side effect, not project publication. Existing project generations and complete results remain unchanged.

If a sink throws, preceding rows may already have been delivered under the existing streaming contract. The private captured result is destroyed and never admitted. Scope cleanup still trims workers. The failure does not imply an all-or-nothing external sink transaction.

## Complete-output capture

The internal selection is produced under the exact Query owner and range. Exact selected row counts and decoded lengths are derived from the immutable snapshot index before output callbacks. Finite output limits are enforced at that point.

The capture candidate allocates row entries and body storage privately, verifies its actual retained capacities against the result-byte budget, then copies emitted bodies. An optional candidate allocation/length failure records `cache_build_failed` and continues delivering the requested output without a candidate. An oversized candidate records `cache_oversized` and also permits normal delivery.

Only a fully delivered candidate reaches `Catalog::Impl::store`. The store allocates its list node before evicting old results, so a store allocation failure after successful delivery preserves old results and reports optional cache failure. Empty selection remains a valid zero-byte cache; a selected empty body still occupies a row entry and consumes row budget.

Capture does not accept a caller-forged selection mask or shape as proof of identity. Predicate evaluation and selection remain internal to Worker. Replay still requires the identical Query owner and requested range.

## Telemetry

| Field | Interpretation |
|---|---|
| `plan_reused` | Prepared query found by exact generation/source identity |
| `worker_reused` | This computation reused an existing worker; false on a complete-result hit |
| `worker_retained` | The worker used by this call survived quiescent trimming; false when no worker was used |
| `worker_oversized` | Observed worker component capacity exceeded the worker-byte limit |
| `worker_scratch_bytes` | Worker component capacity observed before trimming, including transient workers |
| `Stats::worker_entries` | Currently retained workers |
| `Stats::plan_worker_bytes` | Sum of their current component capacities; legacy field name preserved |
| `worker_preparations` | Successful worker constructions |
| `worker_reuses` | Computations that acquired an existing worker |
| `worker_evictions` | Worker releases by retention policy, explicit clear, or plan eviction; includes oversized workers temporarily attached during execution |
| `worker_invalidations` | Workers released by project publication/drop invalidation |
| `worker_oversized_runs` | Computation attempts observed oversized at scope exit, including exceptional exits |

Counter increments saturate at `UINT64_MAX` as the existing counters do. Resource state fields are recomputed from current owners.

### Timing interpretation

`plan_ms` includes prepared-query and worker acquisition. For Capture, the enclosing `run_captured` elapsed duration is partitioned into `execution_ms` and measured `capture_ms`; the latter plus store duration is reported as `cache_build_ms`. The subtraction is clamped at zero to tolerate timer resolution. This keeps the reported partition nonnegative and avoids intentionally double-counting capture inside both fields.

The capture timer includes the exact selection-size scan, candidate allocation and per-row copies. The size scan also enforces output limits and can be needed by an ordinary bounded run. These phase values are diagnostic; they are not an isolated causal measurement of incremental caching cost. Compare complete call/session wall or the established complete-service metric between source-identical Capture and Recompute configurations.

## Regression coverage added

The native catalog suite adds checks for:

- Worker-only clearing with preserved query and result replay.
- Worker reuse separately from prepared-query reuse.
- Entry-based LRU worker eviction while retaining every query.
- Unequal worker sizes and eviction of a complete multiple-victim set.
- Oversized transient workers preserving fitting older workers.
- Zero-byte and zero-entry worker retention limits.
- Sink exceptions after worker creation/decode, followed by cap verification and exact recovery.
- Worker-construction allocation failure with an intact prepared query.
- Exact Capture and Recompute output/replay behavior.
- Three candidate allocation failure points on a warmed query/worker, preserving both successful output and prior cache entries.
- Invalid population enum rejection before callbacks.
- Existing old-lease checks extended to assert no retained worker.

The existing final-callback fault injection now reaches Capture's optional store allocation after all output has succeeded. The payload suite separately covers the lower-level capture implementation. Execution outcomes belong in the build/test receipts; this document describes intended coverage and does not substitute for running the gates.
