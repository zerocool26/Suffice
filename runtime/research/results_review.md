# Suffice 0.12 retention experiment: independent result review

## Finding and scope

The completed run supports a strong, narrowly defined resource result: the observed peak **retained worker pool at API boundaries** falls by 84.68–94.22% under the 256 KiB worker cap, relative to the 16 MiB cap. Capture has favorable paired geometric mean service time in all six Capture/Recompute contrasts, but the strength and consistency vary by workload. Worker churn exposes a reconstruction cost: the tight cap requires 48 worker preparations instead of 12 and has 4.37% greater paired geometric mean service time. These are descriptive results on one immutable corpus and one host, not a general application performance guarantee.

Reviewed evidence: `suffice_v0_12/results/retention_v0_12/SUMMARY.json`, `RUN.json`, and all 112 referenced process records. This review performed read-only arithmetic and record-integrity checks; it did not execute another native benchmark. The separate experiment review describes the original-byte oracle and portable audit boundaries.

| Evidence | SHA-256 |
|---|---|
| SUMMARY.json | `dec47a7039ca58f71ffe4876d4910a0ee76bc79f17787e3bfd0c01f1093320af` |
| RUN.json | `c19d81a246ae332b6d639545538ac3cd1322f3e2bb1e9d22ad315c33a0e263f5` |
| Frozen suite identity recorded by SUMMARY/RUN | `831b16aea5afc343033bf6237ce48708c2d9acc9f0bd4d2b37f6f9709fc36a37` |

Independent calculations verified every process-record hash against RUN, RUN's hash against SUMMARY, successful process status/exit code, request-to-process row and byte totals, retained-pool peaks recomputed from sampled statistics, and `service_ms = raw_session_wall_ms - oracle_audit_ms`. All six paired Capture/Recompute geometric means were recomputed from process records and agree with SUMMARY. Totals are **112 processes, 5,440 complete-output checks, 32,064 rows, and 578,187,424 compared bytes**. Aggregate result-cache counters are 1,984 admissions, 2,688 hits, and 384 evictions.

## Capture versus Recompute

For each repetition, form the matched ratio `Recompute service / Capture service`, then take the geometric mean of the eight ratios. A ratio greater than one favors Capture. Convert this ratio `G` to Capture's service reduction as **`100 × (1 − 1/G)`**, not `100 × (G − 1)`.

| Scenario | Worker cap | Paired service GM ratio | Capture service reduction | Capture wins, service | Raw-wall GM reduction | Capture wins, raw |
|---|---:|---:|---:|---:|---:|---:|
| Reuse | 16 MiB | 1.02710 | 2.64% | 4/8 | 2.11% | 3/8 |
| Reuse | 256 KiB | 1.04631 | 4.43% | 6/8 | 4.07% | 6/8 |
| Pressure | 16 MiB | 1.17237 | 14.70% | 8/8 | 14.34% | 8/8 |
| Pressure | 256 KiB | 1.09007 | 8.26% | 7/8 | 7.99% | 7/8 |
| Mixed queries | 16 MiB | 1.08034 | 7.44% | 6/8 | 7.34% | 6/8 |
| Mixed queries | 256 KiB | 1.08027 | 7.43% | 8/8 | 6.96% | 8/8 |

Pressure with the large worker budget has the strongest directional consistency, but its paired ratios range from 1.04307 to 1.65095. Its paired geometric mean is 1.17237, its median **paired** ratio is 1.12294, and the ratio of its two separately calculated service medians is 1.09765. These are different estimands and must not be substituted for one another. The large high-ratio pair remains part of the reported evidence; there is no justified post hoc exclusion.

Reuse with the large budget is mixed evidence: Capture wins only four service pairs and three raw-wall pairs. Its raw-wall median paired ratio is 0.99653, despite a favorable raw-wall geometric mean. Mixed queries with the tight budget wins all eight service pairs, but the smallest ratio is 1.000096, almost a tie. Directional counts and observed ranges do not establish statistical significance.

The descriptive factorial interaction, defined as `(Recompute/Capture under tight)/(Recompute/Capture under large)` using paired geometric means, is 1.01870 for reuse, 0.92980 for pressure, and 0.99993 for mixed queries. In this run the relative Capture advantage decreases under pressure's tight cap and is almost unchanged for mixed queries. These are descriptive contrasts, not causal conclusions about other workloads.

## Retained worker memory and reconstruction

The following are observed maxima of `plan_worker_bytes` after calls, not the configured caps themselves. Each value is identical across the eight repetitions and, where applicable, across Capture and Recompute.

| Scenario | Observed peak, 16 MiB cap | Observed peak, 256 KiB cap | Reduction |
|---|---:|---:|---:|
| Reuse | 1,693,703 B | 256,505 B | 84.86% |
| Pressure | 1,693,703 B | 259,548 B | 84.68% |
| Mixed queries | 4,530,448 B | 261,737 B | 94.22% |
| Bypass worker churn | 1,693,703 B | 259,548 B | 84.68% |

These numbers describe the named retained-worker component. They exclude other runtime allocations, result storage, consumer allocations, allocator overhead, process RSS, and transient in-call peaks. Therefore “94.22% lower retained worker pool in mixed queries” is supported; “94.22% lower application memory” is not.

In reuse and mixed queries, complete-result hits bypass worker execution. Worker preparations stay at 12 and 36 respectively under both caps, while more workers are evicted. Those traces therefore do not expose sustained worker reconstruction after every eviction. Pressure raises preparations from 12 to 14.

The bypass churn trace disables result retention and preserves all 12 plans. Tightening the worker cap changes preparations **12 → 48**, worker reuses **36 → 0**, evictions **0 → 47**, and final retained worker entries **12 → 1**. Its paired geometric mean `large/tight` service ratio is 0.958094, equivalent to **4.37% greater tight-budget service time**. Raw wall gives **4.11% greater** time. Timing is noisy: tight wins four service pairs and three raw pairs; the service ratio range is 0.78361–1.15383. The deterministic extra preparations establish the reconstruction mechanism more directly than the small descriptive timing difference.

## Interpretation limits for the release

- Each contrast has **eight matched fresh-process blocks**. The 5,440 output checks are correctness coverage within those processes, not 5,440 independent timing replications. Repetition on the same host does not establish independent identically distributed machine conditions.
- Service subtracts explicitly timed oracle/audit work, including full-byte checks, descriptor work, LRU-model checks, and resource audits. Raw wall retains that work. The broadly similar raw-wall estimates are a sensitivity check; subtraction cannot remove observer effects on caches, scheduling, or allocation.
- Admission, startup, output delivery, and other session work remain within the measured boundary. Native phase counters overlap with that boundary and are instrumented; do not add them to service time or characterize the Capture effect as an isolated memcpy result.
- The balanced Williams order controls scheduled position and first-order carryover balance, but does not eliminate host drift or all temporal dependence. No confidence intervals or population-level significance claims are supported by this run.
- Capture and Recompute are ablations in the same 0.12 binary and frozen corpus. These are not comparisons with historical 0.11 timestamps, a native Windows test, a compiler end-to-end trial, or evidence for application-wide 10× speedups.
- The resource effect is clear and the latency tradeoff is workload dependent. A production generalization needs additional independent corpora, longer sessions, realistic consumers, broader output sizes, and representative hosts; none should be implied by this result package.

**Review disposition:** the reported paired arithmetic, component-memory reductions, process totals, and integrity relationships agree with the raw records. Release language should preserve the distinctions above and report worker churn alongside favorable Capture results.
