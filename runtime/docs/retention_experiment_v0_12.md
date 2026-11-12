# Capture and retained-worker experiment — Suffice 0.12

## Purpose and status

`scripts/benchmark_retention_v0_12.py` prepares a new, immutable-evidence experiment and compares complete service under two population strategies and two worker budgets. It changes neither the preserved 0.11 release nor the competition paper. Creating this harness is not evidence that the experiment has run or that either strategy is faster. A completed run requires passed `RUN.json` and independently recomputed `SUMMARY.json`.

The comparison isolates configuration arms of the same 0.12 binary. It does not compare a newly timed 0.12 run with old 0.11 timestamps. Recompute is the retained multi-pass comparator; Capture is the new population strategy. Their native phase timers are diagnostics, not causal cost estimates: Capture's allocation/copy timing includes per-row clock instrumentation, and subtraction cannot erase instrumentation effects.

## Run

From a settled 0.12 project with a completed nonsanitized native build:

```bash
python3 scripts/benchmark_retention_v0_12.py \
  --binary build/catalog_release/suffice_catalog \
  --evidence-source ../suffice_v0_11/results/catalog_sessions_v0_11 \
  --output results/retention_v0_12 \
  --repeats 8
```

The output directory must not exist. The tool never resumes an interrupted experiment or mixes observations with an existing run. Repeats must be a positive multiple of four, at least four; the planned run uses eight. All ordinary output is consumed as hex bodies, decoded by the Python client, and compared against original bytes after every request.

A completed evidence bundle can itself be supplied as `--evidence-source`. The harness prefers its unchanged `EVIDENCE_SOURCE_SUITE.json` and `EVIDENCE_SOURCE_RUN.json` provenance copies and its included `inputs/` tree. This produces new observations in another fresh output directory and requires no original absolute 0.11 path.

The required native options were checked against `apps/catalog/main.cpp`: `--population`, `--max-worker-bytes`, `--max-worker-entries`, `--max-plans`, `--max-cache-bytes`, `--max-cache-entries`, `--max-snapshot-bytes`, `--max-metadata-bytes`, and `--max-projects`.

## Fixed corpus and oracle

The input is the exact preserved 103-file corpus and twelve immutable stores in the earlier experiment's `inputs/` tree. Preparation verifies all 187 recorded input identities, the source suite identity, and the source run's passed status, then copies that tree without recapture or recompression. Old capture reports retain their historical paths as evidence; the new experiment does not dereference those paths.

`source_files` must be sorted by UTF-8 relative-path bytes and preserve the original zero-based modulo-twelve partition. The oracle reads the copied original source bodies directly, assigns row IDs by each project's independently sorted relative paths, and selects by actual byte length. It never uses the captured length feature, selection mask, or native digest as its source of truth. Every result is compared for row count, row order, ID, path, length, and every body byte.

Raw records retain row/path/length/SHA-256 descriptors, not copies of all emitted hex data. Accordingly, the later portable auditor verifies those descriptors and saved observations against the preserved original bytes. It does not replay historical native output or establish that historical timing repeats on a different machine.

## Scenarios and arms

| Scenario | Requests per process | Sequence and predicate | Arms |
|---|---:|---|---|
| `reuse` | 48 | Projects 0–11, four cycles; byte length > 1,024 | Capture/Recompute × large/tight workers |
| `pressure` | 26 | `[0,1]` four times; scan 2–11; `[0,1]` four times; length > 1,024 | Capture/Recompute × large/tight workers |
| `mixed_queries` | 72 | Thresholds 1,024, 8,192 and 65,536, each across all twelve projects; repeat the 36-query sequence twice | Capture/Recompute × large/tight workers |
| `worker_churn` | 48 | Same sequence as reuse, result bypass | Large/tight workers only |

The first three scenarios use `populate`; churn uses `bypass`. Bypass still uses the worker/plan mechanism but performs no result-cache lookup or admission. The population flag is set to Capture for both bypass arms and has no result-population role there.

Large worker budget: **16 MiB**. Tight worker budget: **256 KiB**. Both allow at most **32 retained workers**, **64 compiled plans**, and **64 complete-result entries**. Snapshot budget is **64 MiB**, metadata budget **8 MiB**, and project-slot limit **12**. These are named component limits; no RSS or peak transient-memory claim follows.

Result capacity is independently predicted as `24 × selected_rows + selected_plaintext_bytes`. This describes the experiment's result-row representation and is checked against every reported result capacity. A platform whose allocation/layout behavior disagrees fails the experiment instead of silently accepting a different budget.

- Reuse budget: sum of the twelve selected-result capacities.
- Pressure budget: sum of the capacities of hot projects zero and one.
- Mixed-query budget: sum of all 36 project/threshold result capacities.
- Churn budget: zero result bytes; bypass retains no results.

The result policy is checked using a separate byte/count LRU model after every query. It models full keys `(project, threshold)` because all requests use the complete fixed range and an immutable generation. It verifies hits, misses, admissions, victim sets, aggregate capacities, result count, and cumulative native counters. Zero-byte results still consume an entry.

Worker component bytes and entry counts are checked after startup, each admission, and every query using a STATS response. The peak in the report is the maximum **observed retained state at those boundaries**; it is not an allocation-profiler peak. An oversized transient worker may legitimately exceed the retained-worker budget during execution and then be released.

## Balanced order and machine conditions

The four configuration IDs are:

| ID | Configuration |
|---:|---|
| 0 | Capture, large worker budget |
| 1 | Recompute, large worker budget |
| 2 | Capture, tight worker budget |
| 3 | Recompute, tight worker budget |

Within each four-arm scenario, repetitions use the Williams orders `[0,1,3,2]`, `[1,2,0,3]`, `[2,3,1,0]`, and `[3,0,2,1]`, repeated for eight repetitions. Churn alternates large/tight order. Scenario order rotates once per repetition. Every arm is a fresh native process, including twelve admissions; each repetition yields fourteen processes. The default design therefore yields **112 processes and 5,440 complete-output checks** if fully successful.

The Python runner pins itself to one permitted logical CPU and verifies each child inherits that singleton affinity. Original runner affinity is restored in a `finally` path. Run mode requires the corresponding affinity APIs; audit/summarize are portable standard-library paths. The host CPU is shared, and filesystem caches are not flushed. Balanced order reduces a specific ordering bias; it does not create exclusive hardware or remove all interference.

## Timing contract

The primary measurement is:

```text
service_ms = raw_session_wall_ms - oracle_audit_ms
```

Raw wall time spans child creation through successful shutdown. It includes readiness, all twelve full admissions, plan/worker acquisition, execution, Capture/Recompute work, JSON/hex transmission, Python decoding and materialization, STATS after each admission/query, control bookkeeping, and shutdown.

Only explicitly measured full-output comparisons, descriptor hashing, result-model checks, and resource-audit intervals are subtracted. A few untimed assertions and measurement bookkeeping remain included. One-time evidence copying, frozen-identity checks outside process sessions, oracle loading, and writing raw records after close are outside the session boundary. Both raw and adjusted times are retained.

Subtracting audit duration does not undo audit effects on caches, allocation state, or scheduling. Native phase sums are retained to explain behavior but must not be presented as a clean estimate of the causal speedup from removing one phase.

## Identity and failure gates

The native binary must appear in its adjacent `build_manifest.json`, which must report passed tests, `input_stability: unchanged`, identical before/after input maps, and a nonsanitized build. Every compiled project input listed in that manifest is rehashed. The current Python client and this harness are also hashed. All these project files are copied under `measurement_source/`, and the unchanged build manifest is retained as `MEASURED_BUILD_MANIFEST.json`.

Live project sources, binary, manifest, and copied inputs are checked before preparation completes, between scenario groups, and after all processes. Changed identities fail the experiment. Compiler/SDK/codec/shared-library details remain in the build manifest; the project-input check is not a claim to recapture the entire operating-system environment.

Failures create a failed record/run and stop further measurements. Existing evidence is never relabeled as a new successful run. A new attempt needs a new directory. Correctness regressions and sanitizer runs are separate prerequisites, not mixed into the performance observations.

## Portable audit and summary

```bash
python3 scripts/benchmark_retention_v0_12.py audit --output results/retention_v0_12
python3 scripts/benchmark_retention_v0_12.py summarize --output results/retention_v0_12
```

These modes need Python's standard library and the evidence directory. They do not run native binaries or read historical absolute source paths. They rehash the input inventory, frozen project/Python sources, original suite/run copies, build manifest, suite, and every process record; reconstruct the byte oracle and LRU model; check scheduling, resource limits, counts, phase sums, timing subtraction, and paired aggregation.

If a summary already exists, it must match recomputation exactly. Audit does not overwrite it. Summarize writes a missing summary only. Neither mode resumes a failed measurement.

The summary reports per-variant distributions and two separately defined comparisons:

- `Recompute service / Capture service`: above one favors Capture at the same worker budget.
- `Large-budget service / Tight-budget service`: above one favors Tight at the same population strategy.

The factorial interaction is also retained per repetition: `(Recompute_tight / Capture_tight) / (Recompute_large / Capture_large)`. Above one means Capture's relative service advantage grows under the tight worker budget. It is a descriptive ratio of paired ratios, not a significance test.

All these contrasts, including the interaction, are also computed from raw session wall times under `raw_` keys. Those observations include the explicitly timed audits and allow the effect of the service-time subtraction to be inspected directly.

Worker peak reduction is `1 − tight_observed_peak / large_observed_peak`, reported separately from latency. Report raw ratios, geometric means, medians, observed ranges, and directions by process repetition. The 5,440 requests are correlated repeated work, not 5,440 independent performance trials. No confidence interval or universal speedup is claimed.

## Interpretation boundaries

This is one corpus, partitioned into twelve logical projects, with three simple predicates and no mutation or concurrent clients. The tight budget intentionally trades worker reuse against retention. Lower retained components can remain useful even when service becomes slower, but the two outcomes must be reported together. Larger workloads, native Windows execution, process RSS, actual application integrations, and out-of-process fairness require their own experiments.
