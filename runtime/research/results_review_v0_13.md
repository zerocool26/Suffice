# Independent results review — Suffice 0.13 binary response transport

## Disposition and scope

**No blocking arithmetic, pairing, accounting, or source-identity mismatch was found.** The retained evidence supports a bounded claim: in this recorded Linux experiment, binary responses reduced complete-session stdout bytes by about 49.45–49.59% in the four nonempty-output scenarios, and every one of their 40 matched pairs favored binary on both recorded session timing measures. The empty-output control showed no consistent timing advantage and sent more bytes.

This review independently read the retained process records and original bodies, recomputed the published statistics, reconciled the wire and timing totals, and checked the measured file identities. It did not import the benchmark's auditing implementation, rerun a native process, rebuild the program, or collect new timing observations. The companion JSON preserves the recomputed pair values, order strata, checked invariants, totals, and evidence hashes.

The original benchmark compared each returned row ID, path and complete body against independently read original bytes. The retained process records contain those comparison results and body descriptors, rather than captured raw response streams. This review regenerated the descriptors from preserved originals and checked all 4,840 recorded comparisons. It cannot independently reobserve past emitted bytes; that distinction must remain clear when describing reproducibility.

## Evidence and pairing checks

The review confirmed all 100 process records and their recorded SHA-256 identities, the exact process-file inventory, and their bindings to the shared measured code identity. All records show success and zero child exit status. Recomputing the totals yielded:

| Quantity | Retained and independently recomputed total |
|---|---:|
| Fresh process sessions | 100 |
| Matched JSONL/binary pairs | 50 |
| Complete-output request checks | 4,840 |
| Returned rows checked, including repeated delivery | 23,480 |
| Body bytes checked, including repeated delivery | 419,571,140 |
| Native stdout bytes consumed | 639,715,576 |
| Native stdin bytes sent | 1,035,660 |
| Result-cache hits | 1,680 |
| Result-cache admissions | 1,240 |
| Result-cache evictions | 240 |

The corpus contains 103 original files totaling 1,460,207 bytes, divided deterministically among twelve logical projects. Repeated requests and processes account for the larger delivered-byte total. The twelve projects are partitions of one corpus, not twelve independently sampled repositories.

Each scenario has ten adjacent fresh-process pairs. JSONL runs first in five pairs and binary runs first in five. Scenario position rotates; every scenario occurs at each of the five positions twice for each transport. The schedule records, invocation options, policies, cache budgets, query sequences, initial state, every query's recorded resource statistics, complete output descriptors, and LRU transition records agree across corresponding transport runs. Request-byte totals also match. All recorded parent and child affinities are the same single logical CPU, and restoration is recorded as successful.

The pair is the unit used for the reported ratio. The thousands of requests inside those processes are not additional independent timing trials. The deterministic order balance reduces a first-versus-second transport confound; it is not random sampling of machines, cache states, or execution conditions.

## Recomputed primary results

For repetition r, the paired timing ratio is JSONL time divided by binary time. A ratio above one favors binary. The primary recorded service measure is:

`service_ms = spawn-through-close raw_session_wall_ms - explicitly timed oracle/audit intervals`

It includes process readiness, twelve admissions, queries, Python materialization, STATS exchanges, shutdown and remaining bookkeeping. One-time evidence preparation and record serialization after session close are excluded. Subtracting measured checking intervals does not undo their cache, allocator, scheduling or other observer effects. The unadjusted raw-session result is therefore shown alongside it.

| Scenario | JSONL median service ms | Binary median service ms | Median paired service ratio | Paired service range | Binary service wins | Median paired raw ratio | Median stdout-byte reduction |
|---|---:|---:|---:|---:|---:|---:|---:|
| Reuse | 75.171 | 34.757 | 2.1330 | 1.9995–2.2949 | 10/10 | 2.0488 | 49.5887% |
| Cache pressure | 53.191 | 29.857 | 1.7734 | 1.6542–2.1384 | 10/10 | 1.7257 | 49.5739% |
| Mixed queries | 81.079 | 42.164 | 1.8912 | 1.5565–2.3240 | 10/10 | 1.8201 | 49.4545% |
| Computed / bypass | 78.512 | 40.187 | 1.9885 | 1.6639–2.5940 | 10/10 | 1.9196 | 49.5893% |
| Empty-output control | 22.215 | 22.563 | 0.9912 | 0.9152–2.0117 | 4/10 | 0.9924 | -2.2961% |

The two median-time columns describe each transport separately; the paired-ratio column is the median of ten individual ratios. Dividing the two displayed medians produces a different statistic and should not replace the paired result. The reported ranges are observed minima and maxima, not confidence intervals. No pairs were excluded or trimmed.

Across the four nonempty scenarios, the median paired service ratios span 1.7734–2.1330. Their corresponding median raw-session ratios span 1.7257–2.0488. Every nonempty pair favors binary on both measures, including the pair with the smallest raw advantage. The observed gains therefore do not depend solely on subtracting verification work.

The median ratios for sums of query-request times are 3.7763 for reuse, 3.2071 for pressure, 2.9026 for mixed queries, and 3.0062 for computed/bypass. These are useful diagnostics, but they exclude startup, admission, STATS and shutdown. They must not be presented as whole-session speedups. Native phase clocks overlap client clocks and are not a decomposition into isolated causal costs.

## Empty-output control and wire accounting

The empty-output trace returns no row frames. Its median paired service ratio is 0.9912, binary wins four of ten pairs, and its median raw ratio is 0.9924. These observations do not establish a universal binary timing advantage when there is no body to transfer.

Binary increases complete-session stdout bytes in every empty-output pair: the median increase is 2.2961%, with an observed range of 2.2839–2.3127%. Each empty session has 122 terminal responses. Replacing a one-byte JSONL terminator with a sixteen-byte binary header implies 1,830 additional framing bytes if terminal payload lengths are identical. The measured increases range from 1,825 to 1,848 bytes. The small remainder is consistent with variable lengths of serialized timing values; this is an interpretation of the framing formula and retained metadata, not an independently captured byte-level diff of the terminal streams.

For nonempty rows, the independent size calculation uses 48 bytes of binary framing/prefix plus UTF-8 path bytes and raw body bytes. The JSONL calculation uses its exact decimal and JSON syntax sizes, UTF-8 path encoded as hex, and twice the body length. Every retained row-wire count matches its respective formula.

For every process, the sum of startup, OPEN, STATS, RUN and QUIT response counters equals the cumulative received count. Every response partitions into row and terminal/control bytes; every request sequence is present exactly once; sent bytes reconcile as well. The source and recorded close result include QUIT. Thus the reported roughly 49.5% saving is complete-session stdout representation saving, not a body-only estimate. It does not measure physical storage traffic, kernel copies, network packets, RSS or allocation volume. Terminal lengths are retained measured counters; this audit independently recomputed row lengths and reconciled the terminal counters without claiming to reconstruct their original raw serialization.

## Order effects and outliers

The same service comparison separated by first transport is:

| Scenario | Median ratio when JSONL runs first, 5 pairs | Median ratio when binary runs first, 5 pairs |
|---|---:|---:|
| Reuse | 2.1650 | 2.1009 |
| Cache pressure | 1.7331 | 1.8138 |
| Mixed queries | 1.9260 | 1.8769 |
| Computed / bypass | 2.1600 | 1.9135 |
| Empty-output control | 1.0226 | 0.9856 |

All nonempty gains remain in both order strata. The computed/bypass ratio changes from 2.1600 when JSONL runs first to 1.9135 when binary runs first; pressure changes in the opposite direction. These small strata show that execution order and time-local variation matter. They do not identify which component caused the difference, and five observations per stratum do not justify precise population estimates. The empty-control stratum medians lie on opposite sides of one.

Two observations particularly affect interpretation:

1. **Empty-output repetition 2.** `024_empty_r02_jsonl.json` records 43.346897 ms of service versus 21.547500 ms in `025_empty_r02_binary.json`, producing a 2.0117 ratio. JSONL admission-request time is 32.004985 ms versus 15.279016 ms, and its recorded STATS time is 7.679875 ms versus 3.457669 ms. The query ratio is only 1.2946. These clocks locate much of the difference outside empty query output, but cannot prove a scheduling, filesystem, CPU or other cause. This pair raises the empty arithmetic mean ratio to 1.1016 despite a median below one and only four wins. For sensitivity only, the other nine ratios have a mean of 1.0005. The pair remains in every reported result; the sensitivity calculation is not an alternative trimmed result.

2. **Pressure repetition 5.** `052_pressure_r05_binary.json` records 19.259991 ms of timed audit work, including 16.079226 ms at query index 18. The paired JSONL process records 3.800656 ms of audit work. Consequently, the pair's raw ratio is 1.1246 while its audit-adjusted service ratio is 1.6988. The timing contract excludes that interval explicitly, so the subtraction is arithmetically correct. It is still material enough that a report should preserve raw and adjusted results rather than implying they are interchangeable. No cause is assigned and neither record is excluded.

Computed/bypass repetition 0 also has the largest service ratio in that scenario, 2.5940, alongside 29.489284 ms of JSONL admission time versus 15.469045 ms for binary. This reinforces why the full distribution, paired statistic and clear session contract matter more than selecting a maximum.

## Source-freeze stability

All 78 frozen measurement-source files match their recorded hashes. The corresponding current project files also match those hashes at the time of this review. The 187 retained input files match their exact inventory, byte lengths and SHA-256 hashes. The current native binary and build manifest match the measured identities, and the preserved build-manifest copy has the same hash. The stored before/after code and input identities agree with the prepared suite. The predecessor-suite and predecessor-run copies also match their declared provenance hashes.

The measured client hash is `f0282195bf939aded456aa54258bcf0c237ccd9255660d665875feaaf6680495` and the measured binary hash is `b5b0c7a8d767441dfe023150f0f8d49fb223d4a669e61ea7896c7dd53ed62465`. The harness hash is `edd889bc32902304a79c2e719174841cc505567281272e703b4cd55831f6a3c9`. These identify the reviewed measurement implementation rather than a later edited implementation.

| Evidence file, relative to `results/transport_v0_13/` | SHA-256 |
|---|---|
| `MEASURED_BUILD_MANIFEST.json` | `aeb514b964e7a4a0c767ac107c02bf3b60c14689fae3f983571e910cd8f57cbf` |
| `RUN.json` | `dc9538f23416d248dbb49bc2f6462f86ab335ddca2bc8f56ee7337d4a1cd61a5` |
| `SUMMARY.json` | `b8923214bf1209b681e870fb305294ef00a90d044c829ab4594cc8c087469a04` |
| `suite.json` | `ce20f82b89b3f6b94122f7672be73e1c933d452a630ee871c68afb9470b9d57c` |

Equality of these retained identities supports consistency of this local record set. It is not continuous attestation of the machine or a proof that environmental interference was absent.

## Publishable wording and boundaries

A precise result statement is:

> On one recorded Linux configuration, ten balanced fresh-process JSONL/binary pairs per scenario used the same frozen 103-file corpus and identical output, cache and retention contracts. In the four nonempty-output scenarios, binary response framing reduced measured complete-session stdout bytes by 49.45–49.59% at the median. Median paired JSONL/binary service ratios were 1.77–2.13, and median unadjusted session ratios were 1.73–2.05; all 40 nonempty pairs favored binary on both measures. The empty-output control showed no consistent timing gain and increased stdout bytes by about 2.30%.

That claim is supported by the retained data. It should be accompanied by the service-time definition and the complete paired observations, including the negative control and outliers.

The data do not establish a general twofold runtime speedup, independent CPU cost of serialization, cross-platform performance, bounded wall-time against a stalled peer, memory savings, effects on unrelated queries, or a ranking against other runtime systems. All measured query traces use one body-length predicate family over one small frozen corpus, a shared pinned logical CPU without cache flushing or exclusive ownership, and the instrumented local-pipe Python consumer. Repeated requests improve the precision of a recorded session's accounting; they do not create independent workload diversity.

The review supports publication of the bounded result above with those stated limits. No new native execution was needed to resolve a numerical or identity discrepancy.
