# Suffice 0.12: exact reuse with independently bounded execution state

## Executive result

This release advances New Runtime Tech with two implemented changes, a complete controlled experiment, and a deeper architecture for future incremental execution. Prepared queries can now survive without retaining their execution workers. Explicit result population now captures complete output during normal decoding, avoiding the two additional decoding traversals used by the preserved construction control.

The new experiment completed **112 fresh native processes and 5,440 exact output checks**. A 256 KiB worker-retention budget reduced the observed peak of the retained-worker component pool by **84.68–94.22%**, depending on the workload, relative to the 16 MiB configuration. Capture had lower paired-geometric service time in all six construction comparisons, with reductions from **2.64% to 14.70%**. Individual timing observations were mixed in several comparisons. Under bypass-only worker churn, the tight budget reconstructed 48 workers instead of 12 and had **4.37% longer paired-geometric service time**. This is a measured resource/latency tradeoff on the supplied workload, not a universal speedup or a process-RSS reduction.

The strongest architectural conclusion is that **validity, representation, and retention value require separate decisions**. Query reuse is not worker reuse; complete-result reuse is not cross-generation validity; a cache hit is not necessarily a faster useful session. The implementation now makes one of those separations operational. The accompanying research develops the next ones with explicit assumptions, counterexamples, and experiments.

**Evidence run:** 2026-11-12, 04:53:59–04:54:09 UTC. The preserved 0.11 source, measurements and competition manuscript remain separate historical artifacts. The current experiment compares configurations of the same 0.12 binary; it does not replace the paper's earlier Bypass/Populate study.

![Paired service observations and retained-worker capacity](figures/Suffice_0_12_Results.png)

*Figure 1. Every timing dot is one matched process block. The horizontal spans are observed ranges, not confidence intervals. Memory values are the maximum observed retained-worker state at checked operation boundaries. The lower bars describe Capture for the first three scenarios and Bypass for worker churn; Recompute has the same retained peaks in the corresponding scenarios.*

## 1. The preserved foundation and the gaps addressed

Suffice is a C++20 and Python research runtime for exact operations over immutable artifact generations. Its earlier computational work separates the information needed for a result from the representation used to execute it. Its artifact layer captures original bytes and versioned features, performs semantic admission checks, evaluates exact integer predicates, selectively decodes payloads, and exposes complete ordered results through a persistent native service. Publication revisions and leases keep old captured versions explicit.

This release concentrates on the resource and construction costs exposed by the 0.11 experiment. It preserves the earlier computational substrate, capture/publishing code, generation validation, range handling, exact cache identity, and full-output contract. It does not broaden the runtime into an arbitrary build engine or concurrent operating system.

The review identified five concrete gaps:

| Gap | Consequence | Resolution or current boundary |
|---|---|---|
| A retained plan also retained a full mutable Worker | Keeping a prepared query kept decoder/executor capacity even when results replayed directly | Query and Worker retention are now independent |
| Populate delivered output and then decoded it twice more to build a cache | Construction paid work already performed for the useful answer | Capture builds a private candidate during normal decoding |
| A worker can grow during execution | A constructor-only budget check can become stale | Allocation-free scope cleanup remeasures and trims on successful and exceptional exits |
| Hit counts and phase timers can hide complete service costs | An apparently faster internal operation may not improve a useful session | The experiment includes admission, construction, complete transfer/materialization and shutdown |
| More ambitious reuse lacked a complete validity/ownership contract | Unchanged file bodies can coexist with changed answers or unreclaimed owners | The research specifies observation witnesses, shared-allocation accounting and reservation obligations; these remain proposals |

The first three are executable changes. The fourth is addressed by measurement design. The fifth supplies a falsifiable next development direction and is not described as an implemented capability.

## 2. Implemented engineering

### 2.1 Independent Worker retention

A retained plan now owns its immutable Query independently and may own a Worker. Worker eviction releases decoder and execution scratch while preserving the prepared query. A later computed request constructs a Worker from that same Query. A complete-result hit acquires no Worker and does not refresh worker recency.

New native limits are `max_worker_bytes` and `max_worker_entries`, defaulting to 16 MiB and 32. They are independent of the prepared-plan entry and source-text limits. A zero byte or entry cap permits computation but disables Worker retention. An individually oversized Worker executes transiently and is released at the settled call boundary. `Catalog::clear_workers()`, wire command `CLEAR workers`, and the Python client's `clear("workers")` release Workers while retaining prepared queries and complete results.

Let W be the Workers attached to retained plans after an API operation returns or throws, and let b(w) be the current reported component capacity. The implemented invariant is:

\[
|W|\le N_W,\qquad \sum_{w\in W}b(w)\le B_W.
\]

Cleanup first removes individually oversized Workers. It then retains the newest fitting prefix in the computed-access ordering, releasing older Workers until both caps hold. Arithmetic subtracts accepted sizes from remaining headroom. The cleanup scan allocates no storage and executes during exception unwinding as well as normal completion. This matters because decoder contexts can grow after construction.

This is a **quiescent retained-component bound**. It is not a bound on transient allocations, complete-result candidates, Query internals, consumers, allocator overhead, mapped pages or process RSS. The Worker still eagerly prepares its supported execution structures. Lazy backend preparation is a separate possible change.

### 2.2 Complete-result capture during normal decoding

`PopulateStrategy::Capture` is now the default for explicit Populate. `PopulateStrategy::Recompute` preserves the earlier construction path as an ablation control. Default Use behavior remains explicit: a miss computes but does not automatically populate a result.

Capture performs these steps:

1. Compute the exact selection under the immutable Query and requested range.
2. Read selected row counts and decoded lengths from that same snapshot's index.
3. Enforce finite output limits before invoking the sink.
4. Privately allocate row entries and body storage if the result budget permits.
5. Decode each selected body, deliver it to the sink, then copy it into the candidate after the callback returns.
6. Return a candidate only after every selected row has completed and its extent has been checked.
7. Stage the catalog's result-list node before evicting any existing results.

The exact generation/source/range identities and Query-owner checks are preserved. Selection remains internal to the Worker; a caller-supplied mask cannot certify the identity of a result.

The optional candidate has a defined failure policy. A candidate allocation or length failure records cache-build failure and continues normal output. An oversized candidate also permits uncached output. A sink or decoding exception destroys the candidate and prevents publication. Previously emitted rows can remain visible to the sink under the existing streaming contract; this does not promise atomic external output. A post-output store allocation failure preserves old cache entries because the replacement node is staged before eviction.

Capacity checks protect both logical size arithmetic and actual retained vector capacities. Empty selection is a valid zero-byte result; an included empty body still needs a row descriptor. Capture can increase transient live memory by allocating a candidate before output completes. A stronger whole-call memory reservation scheme remains future work.

### 2.3 What the decode and timing claims mean

The Recompute control performs normal delivery followed by two cache-construction body traversals. Capture eliminates those two extra construction traversals by sharing normal delivery. That structural fact follows from the code paths.

The existing pipeline decoded-byte counter covers normal delivery and omits the control builder's additional traversals. It therefore cannot be used to report a measured total decoder-byte reduction. Capture's diagnostic timer includes the selected-size scan, candidate allocation, per-row copying and associated instrumentation; the scan also serves finite output-limit enforcement. These phase counters are not an isolated causal estimate of incremental cache cost. The performance comparison uses complete service and raw wall time.

Details and source-level failure behavior are recorded in [the Worker/Capture contract](docs/worker_retention_v0_12.md). The independent AI-assisted [Capture review](research/capture_review.md) records exact inspected source hashes; it is a scoped code review, not formal verification or external peer review.

## 3. Controlled experiment

### 3.1 Inputs and arms

The corpus is the preserved 0.11 experiment input: **103 source files containing 1,460,207 original bytes**, partitioned into twelve logical projects. It is one code corpus, not twelve independently sampled production repositories. Original bodies, captured stores and provenance records were copied without recapture or re-encoding. Every output oracle reads original bodies and applies a byte-length predicate independently of the runtime's packed features.

The first three scenarios cross two construction strategies with two Worker budgets:

| Factor | Level 1 | Level 2 |
|---|---|---|
| Complete-result construction | Capture | Recompute |
| Retained Worker bytes | 16 MiB | 256 KiB |

Worker entries are capped at 32 in both configurations. Prepared-plan entries are 64 and result entries 64 for this experiment. The fourth scenario disables complete-result retention and compares only the two Worker budgets.

| Scenario | Requests per process | Demand pattern | Complete-result byte budget |
|---|---:|---|---:|
| Reuse | 48 | Four cycles through twelve projects, length > 1,024 | 1,457,164 |
| Scan pressure | 26 | Hot pair, ten-project scan, hot pair again | 283,059 |
| Mixed queries | 72 | Three length thresholds × twelve projects, repeated | 2,955,422 |
| Worker churn | 48 | Reuse order with every request in Bypass mode | 0 |

Mixed thresholds are 1,024, 8,192 and 65,536 bytes. A fresh native process runs each condition. Eight repeated blocks use a balanced four-arm Williams order for the factorial scenarios and alternating order for worker churn; scenario order rotates by block. Runner and child share one permitted logical CPU. The CPU is not exclusively owned, and filesystem caches are not flushed.

### 3.2 Timing and correctness boundaries

The primary measure is:

\[
T_{\mathrm{service}}=T_{\mathrm{spawn\ through\ close}}-T_{\mathrm{explicit\ oracle/audit}}.
\]

It includes readiness, twelve Full admissions, query and Worker acquisition, execution, construction, complete JSON/hex transfer, Python decoding/materialization, statistics requests after admissions and queries, shutdown, and remaining control bookkeeping. Explicitly timed full-byte comparisons, descriptor/digest checks, result-LRU checks and resource audits are subtracted. Preparation/copying and oracle loading occur before sessions. Serializing retained process records occurs after close.

Raw wall times remain available. Subtracting audit time does not undo its effects on cache state, allocation or scheduling. Phase timers remain diagnostic. The principal observations are eight matched fresh-process blocks per comparison, not 5,440 independent timing trials. This report supplies descriptive ratios, ranges and direction counts; it does not provide confidence intervals or a population-level significance claim.

Every measured request compares complete ordered row IDs, paths and body bytes. An independent LRU model checks result hits, admissions, full victim sets, occupancy and counters under the experiment's fixed-generation/full-range contract. Worker budgets are checked after every operation. Frozen evidence includes 187 input files, 74 measurement-source files, the measured build manifest, all 112 process records, schedule, source identities and raw timings. The portable audit validates the retained evidence and arithmetic; it does not recreate historical execution merely by reading those files.

The complete protocol is [documented separately](docs/retention_experiment_v0_12.md). Its [independent review](research/experiment_review.md) records two path-validation corrections made before the measurement freeze.

## 4. Measured results

### 4.1 Output and retention checks

All **5,440** complete-output checks passed across **112** processes. They covered **32,064 rows and 578,187,424 body bytes**. The result model reconciled **2,688 hits, 1,984 admissions and 384 evictions**. These counts include repeated outputs and are correctness/evidence volume, not distinct datasets or independent statistical trials.

No failed run was discarded or rerun to select a favorable timing result. The completed scheduled matrix and all its repetitions are retained. Runner affinity was restored after the experiment.

### 4.2 Capture versus Recompute

For repetition i, define r_i = Recompute service / Capture service. A ratio above one favors Capture. The reported geometric mean is computed over matched ratios, rather than by dividing separate policy medians. Its equivalent service reduction is 1 − 1/GM.

| Scenario | Worker budget | Paired geometric ratio | Equivalent service reduction | Observed ratio range | Blocks favoring Capture |
|---|---|---:|---:|---:|---:|
| Reuse | 16 MiB | 1.0271× | 2.64% | 0.9876–1.0913 | 4/8 |
| Reuse | 256 KiB | 1.0463× | 4.43% | 0.9946–1.1258 | 6/8 |
| Scan pressure | 16 MiB | 1.1724× | 14.70% | 1.0431–1.6510 | 8/8 |
| Scan pressure | 256 KiB | 1.0901× | 8.26% | 0.8221–1.5304 | 7/8 |
| Mixed queries | 16 MiB | 1.0803× | 7.44% | 0.9722–1.2344 | 6/8 |
| Mixed queries | 256 KiB | 1.0803× | 7.43% | 1.0001–1.1401 | 8/8 |

The Scan-pressure/16 MiB comparison favors Capture in all eight blocks, but its large upper observation shows that the average magnitude is sensitive to run variability. Reuse/16 MiB is especially weak evidence for a consistent latency gain: half its service observations favor each construction strategy. Its raw-wall comparison favors Capture in only three of eight blocks, although the raw paired geometric ratio is 1.0215×.

Across all six construction comparisons, raw-wall paired geometric ratios are 1.0215×, 1.0425×, 1.1674×, 1.0868×, 1.0792× and 1.0748× in the same table order. The geometric direction survives including audit time, but that does not remove noise or establish generality. No aggregate is formed by pooling unrelated scenario requests.

Separate policy medians give elapsed-time scale, not the paired effect estimator:

| Scenario | Capture / 16 MiB | Recompute / 16 MiB | Capture / 256 KiB | Recompute / 256 KiB |
|---|---:|---:|---:|---:|
| Reuse | 72.7835 ms | 74.2720 ms | 72.3733 ms | 74.7562 ms |
| Scan pressure | 50.3078 ms | 55.2203 ms | 51.4338 ms | 55.0251 ms |
| Mixed queries | 76.6762 ms | 85.1328 ms | 75.2838 ms | 80.5341 ms |

### 4.3 Retained Worker memory and reconstruction

The table reports the maximum retained-worker component state observed at checked operation boundaries. The same values recur in all eight repetitions. Capture and Recompute have the same peaks for their corresponding scenario.

| Scenario | Peak with 16 MiB budget | Peak with 256 KiB budget | Reduction of this pool | Worker constructions: large → tight |
|---|---:|---:|---:|---:|
| Reuse | 1,693,703 B | 256,505 B | 84.86% | 12 → 12 |
| Scan pressure | 1,693,703 B | 259,548 B | 84.68% | 12 → 14 |
| Mixed queries | 4,530,448 B | 261,737 B | 94.22% | 36 → 36 |
| Worker churn | 1,693,703 B | 259,548 B | 84.68% | 12 → 48 |

The tight peaks remain below 262,144 bytes. The mixed workload retains up to 32 Workers with the generous budget versus at most two with the tight budget. Peak entries and peak bytes can occur at different operation boundaries; these are separate maxima, not one synthetic snapshot.

Reuse and mixed-query traces need no extra Worker construction under the tight budget because later identical requests replay complete results. This demonstrates why result demand and Worker demand should be counted separately. Scan pressure creates two additional Worker constructions after the hot results are displaced.

Worker churn exposes the adverse case directly. With complete results bypassed, the large budget constructs 12 Workers and reuses them 36 times; the tight budget constructs 48 Workers and reuses none. Tight/large paired-geometric service is **1.0437×**, corresponding to **4.37% more service time**; raw wall is **1.0411×**. Four blocks favor each service configuration. Separate service medians are 77.5353 ms for large and 80.4693 ms for tight.

This evidence supports independently configurable resource policy. It does not support silently making 256 KiB the default for every deployment. The shipped default remains 16 MiB; the tight setting is a deliberate option for constrained workloads.

### 4.4 Interactions and interpretation

The factorial interaction is the ratio of Capture's relative advantage under the tight budget to its advantage under the large budget, calculated within each block. Its paired geometric values are 1.0187 for reuse, 0.9298 for scan pressure and 0.9999 for mixed queries. All have broad observed variation; mixed queries split four blocks on each side of one. The experiment does not justify one universal claim that the two changes reinforce or cancel one another.

The result is a locally favorable construction change plus a strong, explicit retention control with measurable reconstruction costs. It is not a demonstrated 10× service improvement, a novel universal eviction policy, or evidence of improved language-model task success.

## 5. Correctness and review evidence

| Gate | Result | Recorded scope |
|---|---|---|
| Native Release build and checks | Passed | Eight executables; nine correctness/CLI checks; source inputs stable |
| Address/undefined-behavior sanitizer build and checks | Passed | Same nine checks; source inputs stable |
| Native Catalog regressions | Passed | 26 groups; 104 successful runs; 39 expected rejections; 12 injected allocation failures |
| Native payload regressions | Passed | 17 groups; 370 pipeline runs; 47 cache replays; 65 expected rejections |
| Full Release integration | Passed | Seven groups; 114 successful commands; 35 expected rejections; 14 corrupted generations rejected |
| Full sanitized integration | Passed | Same seven groups and integration counts |
| Artifact producer | Passed | 22 tests; zero failures/errors/skips; interruption and uncertain-publication cleanup cases included |
| Worker protocol integration | Passed | Capture and Recompute; six complete-output checks through real Python/native pipes |
| Scheduled experiment and retained-record audit | Passed | 112 processes; 5,440 complete outputs; all source/input identities and result accounting checked |

Native Catalog and payload suites are part of the build gates, so those rows are not independent additional test populations. Sanitizer leak detection was disabled; the separately installed codec library was not instrumented. Successful sanitizer runs improve defect detection under the tested paths and do not prove absence of all memory errors.

The added regressions exercise Worker-only clear, query reuse without Worker reuse, byte and entry pressure, multiple Worker victims, oversized transient Workers, zero caps, sink exceptions, constructor failures, candidate failures, post-output store failure, output preflight, wrong-owner/range replay rejection, old leases, empty outputs and corrupt on-demand payloads. The tests check observable output, retained owners and recovery behavior.

Validation records are under `build/retention_release/`, `build/retention_sanitize/`, and `results/`. The package retains manifests, test/build logs and result summaries; large intermediate objects, sanitizer binaries and temporary corruption fixtures are omitted. The tests regenerate those fixtures when executed. Two Linux Release executables are included with their measured identities; rebuilding remains the supported portable path.

## 6. Deeper research direction

### 6.1 Keep the semantic decision separate from the economic decision

Every retained object should carry three independently inspectable contracts:

| Decision | Required contract | Safe consequence of uncertainty |
|---|---|---|
| Validity | Which observations and versioned operation establish this exact answer? | Recompute or reject an unsupported operation |
| Representation | What retained data and reconstruction steps produce the consumer's answer? | Use a complete supported representation |
| Economics | Which future work could retention avoid, at what construction and displacement cost? | Use a bounded simpler policy; correctness remains unchanged |

The research review connects this design to incremental computation and dependency traces, including Adapton and Build Systems à la Carte [1,2]. Their semantics are precedents and proof targets; their guarantees do not automatically apply to Suffice.

For a deterministic task whose complete external observations pass through a fixed versioned interface, equal answers to the complete prior observation trace preserve the task's output. The proof sketch proceeds by induction over observations: equal prior state and answers imply the same next computation and observation. The assumptions include fixed code/parameters, complete observation mediation and a coherent immutable world. This is a statement about that model, not a verified theorem about the current C++ implementation.

The critical missing dependencies are often negative or structural: an absent config file becomes present; a directory gains a source file; a toolchain or extractor changes; or an insertion shifts generation-local row IDs. An unchanged body blob may remain reusable while its old ordered row/path result becomes invalid. A witness budget must return unknown when exhausted, never silently certify an incomplete trace.

### 6.2 Separate paying to build from choosing to retain

A useful explanatory preconstruction condition is:

\[
H(D-R)\ge C+O+L(V),
\]

where H is expected useful future reuse, D direct cost, R replay cost, C remaining construction cost, O policy overhead and L(V) the future value lost by displacing the entire victim set V. Costs require comparable units and a stated horizon. This is not a proved online predictor.

Once construction has finished, C is sunk and should not be charged again when deciding whether to retain the object. Building every candidate before applying admission cannot avoid wasted construction. Counting all future requests as Worker reuse is also wrong when complete-result hits bypass the Worker.

The cache research covers variable-size aggregate-victim admission, simple bounded probation/eviction, cost-sensitive policies and occasional learning outside the hot path [3–6]. The next implementation should start with a bounded inexpensive history baseline. A sophisticated predictor must beat that baseline after its own CPU, metadata, adaptation and cold-start costs are included. No learned controller is implemented in this release.

### 6.3 Account for shared ownership and reserve before allocation

For a fixed graph of retained allocations, let closure(S) contain all allocations reachable from owner roots S. Then:

\[
C(S)=\sum_{u\in\operatorname{closure}(S)}b(u),\qquad
\text{reclaim}(V)=C(S)-C(S\setminus V).
\]

The second expression depends on all surviving owners. Removing one of two entries sharing a payload cannot be credited with freeing the entire payload. External leases belong among those roots. A root's marginal addition becomes smaller as more of its shared dependencies are already retained; eviction value is consequently not a fixed per-entry byte number.

A stronger future controlled-memory invariant is:

\[
\text{live controlled bytes}+\text{outstanding reservations}\le B.
\]

It requires reservations before work, upper bounds or checked incremental growth, atomic conversion of reservation to live charge, and complete participation of allocation paths. Cancellation cannot release a charge while work can still access the allocation. Microsoft's `CancelIoEx` documentation explicitly distinguishes requesting cancellation from completion [7]. That platform behavior matters for a future Windows asynchronous scheduler; the current serialized service does not implement one.

No eviction policy can retain arbitrary independently pinned, incompressible histories in bounded memory without refusing further demand, limiting history, recomputing or using separately budgeted external storage. The API must make that tradeoff explicit.

### 6.4 Respect the useful-output cost floor

Full-body reads, identifiers, pinned handles and deltas have different contracts. A handle-only producer timer cannot be compared with a complete-body consumer timer unless handle resolution is included. A delta requires its correct base and application costs.

Under an additive model, if an unchanged stage accounts for fraction f of baseline elapsed time, removing every other stage gives a ceiling of 1/f. Thus compute-only changes cannot promise a tenfold complete-session improvement if unchanged delivery already exceeds one tenth of elapsed time. The current phase labels do not establish that fraction; a controlled transport experiment must measure it.

This motivates testing binary framing with exactly the same ordered paths and full bodies before using changed-output contracts. It also motivates bounded output queues and backpressure: faster computation should not merely move retained memory into a slow consumer.

## 7. Prioritized next experiments and rejection criteria

| Priority | Experiment | Required oracle and accounting | Evidence that rejects or limits it |
|---|---|---|---|
| 1 | Binary framing versus JSON/hex with unchanged full outputs | Complete downstream bytes/order; admission-to-close time; wire bytes, allocation and RSS | Savings vanish in complete consumption, or decoder/backpressure complexity creates regressions |
| 2 | Shared body/feature values and dependency witnesses across edits | Fresh recomputation under the same immutable manifest; positive, negative, directory and toolchain dependencies | Any stale output, trace-budget escape, or validation cost above recomputation for a workload |
| 3 | Ten-project reservations, shared in-flight work and cancellation | Exact version/demand identity; ownership charges; bounded queues; completion-aware reclamation | Budget violation, early release, stale publication, starvation or uncontrolled slow-consumer growth |
| 4 | Bounded second-touch/cost-aware admission, then more advanced policies | Complete service cost including policy work, full victim sets and cold start | Gains confined to hit count or an uncharged policy/consumer component |
| Target-machine gate | Native Windows build and validation, followed by a separate platform experiment | Executed Windows receipts, exact-output checks, platform-specific time/RSS/disk measurement | Linux-only observations presented as Windows evidence |

Each experiment must keep adverse workloads and failed hypotheses visible. Proposed workloads include one-hit scans, reuse after invalidation, huge candidates displacing many small ones, zero-byte results, query/representation churn, shared pinned owners, slow sinks, cancellation, absent-to-present files, directory changes and toolchain changes. The detailed protocols are in [the foundations report](research/foundations.md) and [cache economics report](research/cache_economics.md).

## 8. Limitations and release decisions

This release keeps Capture as the explicit Populate default because it removes redundant construction traversals, passed failure-focused regressions, and was locally favorable in the controlled comparisons. Recompute remains available as a reproducible control. The generous Worker default is preserved because constrained retention trades memory for reconstruction, with workload-dependent latency. Applications can choose the tight budget deliberately.

The measured scope remains one source corpus, simple length predicates, one Linux toolchain, one shared pinned CPU, eight matched blocks per comparison and instrumented sessions. There are no production traces, broad repository sampling, confidence intervals, power-loss durability tests, whole-process memory guarantees, concurrent execution claims, GPU results, Gemma inference, agent issue-resolution results or native Windows performance measurements here. CMake configuration is included, but the recorded release path uses the direct Python builder.

The native Windows runner is provided and unexecuted on this host. The timing harness requires Linux-style CPU-affinity APIs; its retained-record auditor is portable Python. A fresh Windows run must generate its own evidence before platform-specific claims are made.

## 9. Completed checklist and evidence navigation

- [x] Preserve the compact 0.11 source, manuscript and historical validation lineage.
- [x] Review construction, ownership, exception paths and resource accounting.
- [x] Implement independent Worker retention, quiescent trimming and Worker-only clear.
- [x] Implement complete-result Capture with exact identity and private publication.
- [x] Retain Recompute as a same-binary controlled comparison.
- [x] Add failure-focused native and real protocol regressions.
- [x] Run Release, sanitizer, producer and full integration validation.
- [x] Freeze inputs, measured code and the balanced four-scenario protocol.
- [x] Execute all 112 processes and compare all 5,440 complete outputs.
- [x] Preserve raw wall, audit time, per-block results, negative observations and limitations.
- [x] Review the experimental arithmetic independently and package the complete evidence bundle.
- [x] Specify deeper validity, ownership, reservation and transport experiments with rejection criteria.

Reproduction commands are in [README.md](README.md). `results/retention_v0_12/SUMMARY.json` is the machine-readable result index; process records and hashes provide its detail. `research/results_review.md` records the independent arithmetic review. `RELEASE_INVENTORY.json` identifies every included payload file. `PROJECT_HANDOFF.md` records the continuation state and distinctions future work must preserve.

## Selected primary references

The two JSON research catalogs include sixteen source entries, publication dates, source roles and application boundaries. The following sources support the architectural discussion; their external performance results are not Suffice results.

1. Hammer et al., *Adapton: Composable, Demand-Driven Incremental Computation*, PLDI 2014. [Author manuscript](https://www.cs.umd.edu/~mwh/papers/adapton-submit.pdf).
2. Mokhov, Mitchell and Peyton Jones, *Build Systems à la Carte: Theory and Practice*, Journal of Functional Programming, 2020. [Author publication page](https://www.microsoft.com/en-us/research/publication/build-systems-a-la-carte/).
3. Einziger et al., *Lightweight Robust Size Aware Cache Management*, ACM Transactions on Storage, 2022; earlier 2021 preprint. [Primary preprint](https://arxiv.org/abs/2105.08770v2).
4. Yang et al., *FIFO Queues Are All You Need for Cache Eviction*, SOSP 2023. [Author paper](https://www.cs.cmu.edu/~rvinayak/papers/s3-fifo-sosp-2023-fifo-queues-are-all-you-need-for-cache-eviction.pdf).
5. Zhang et al., *SIEVE is Simpler than LRU: an Efficient Turn-Key Eviction Algorithm for Web Caches*, NSDI 2024. [Official proceedings](https://www.usenix.org/conference/nsdi24/presentation/zhang-yazhuo).
6. Xia et al., *Learning-Augmented Heuristics: Simple Yet Smart, Robust and Interpretable Cache Eviction*, OSDI 2026. [Official proceedings](https://www.usenix.org/conference/osdi26/presentation/xia).
7. Microsoft, *CancelIoEx function*, Windows API documentation. [Official documentation](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).

Project-specific numerical claims are supported by the bundled raw records, build manifests, test logs, frozen source and reviews. Proof sketches apply to their stated models. Implementation tests, reviews and retained-record audits have distinct roles and are not interchangeable guarantees.
