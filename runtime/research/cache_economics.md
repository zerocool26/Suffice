# Retained computation as a constrained resource

## Research decision for the next Suffice runtime

Prepared November 11, 2026, America/Chicago; sources retrieved November 12 UTC. This note reviews the frozen Suffice 0.11 runtime and proposes a subsequent design. It does not claim that its proposed mechanisms have been implemented, benchmarked, or proved correct. Any 0.12 implementation must identify its actual subset and supply separate evidence. The existing competition manuscript remains a report of the earlier measurements.

**Recommended first change:** place retained prepared workers under an explicit byte budget. Complete-result admission and worker retention are different decisions, and they should have different accounting. Then reduce avoidable construction work and compare a bounded, inexpensive admission baseline before introducing a learned controller. A sophisticated score cannot compensate for charging the wrong resources or optimizing the wrong endpoint.

This recommendation follows the local evidence: the measured sessions retained 1,693,703 worker-component bytes versus 444,574 logical snapshot-input bytes; complete-result population did not consistently help the reuse trace and increased paired geometric service time under pressure. Those are findings for one preserved corpus and two traces, not evidence that caching is generally unhelpful. Sources: `suffice_v0_11/docs/catalog_api_v0_11.md`, `docs/catalog_review_v0_11.md`, and the preserved `Suffice_Kaggle_Writeup.md`; exact input identities appear in `cache_sources.json`.

## 1. What the literature changes about the design

The following are primary publisher, author, or institutional sources. Dates are the verified proceedings dates unless explicitly identified as an article publication date. The source catalog distinguishes those date types. External performance figures are deliberately omitted: they are not Suffice measurements.

| ID | Research and date | Mechanism to learn from | Boundary when applying it here |
|---|---|---|---|
| R1 | [Cao and Irani, Cost-Aware WWW Proxy Caching Algorithms](https://www.usenix.org/conference/usits-97/cost-aware-www-proxy-caching-algorithms), December 8–11, 1997 | GreedyDual-Size combines locality, retrieval cost, and object size. | A cheap hit does not repay a separately incurred materialization cost automatically. Define the actual avoided cost first. |
| R2 | [Einziger et al., Lightweight Robust Size Aware Cache Management](https://cris.technion.ac.il/en/publications/lightweight-robust-size-aware-cache-management/), published August 25, 2022; [author preprint](https://arxiv.org/html/2105.08770v2) | The Aggregated Victims variant compares a candidate with the combined frequency of all entries displaced to make room. | Comparing only the first victim can approve a harmful large admission. Construction costs and dependency sharing require additional analysis. |
| R3 | [Yang et al., FIFO Queues are All You Need for Cache Eviction](https://www.cs.cmu.edu/~rvinayak/papers/s3-fifo-sosp-2023-fifo-queues-are-all-you-need-for-cache-eviction.pdf), October 23–26, 2023 | S3-FIFO uses a small probationary queue, a main queue, and bounded ghost history to remove objects with little reuse. | Its resident probationary objects and a metadata-only second-touch gate have different first/second-access behavior. A new baseline must be named accurately. |
| R4 | [Zhang et al., SIEVE is Simpler than LRU](https://www.usenix.org/conference/nsdi24/presentation/zhang-yazhuo), April 16–18, 2024 | SIEVE combines lazy promotion with quick demotion using a queue, a hand, and visited state. | Its web-cache results do not establish a win for a small, caller-serialized catalog. Policy overhead must be measured at this scale. |
| R5 | [Beckmann et al., LHD: Improving Cache Hit Rate by Maximizing Hit Density](https://www.usenix.org/conference/nsdi18/presentation/beckmann), April 9–11, 2018 | Conditional reuse estimates rank expected hit contribution relative to occupied space. | Predicted hits are only a surrogate when different hits avoid different amounts of work. |
| R6 | [Yang et al., CacheSack](https://www.usenix.org/conference/atc22/presentation/yang-tzu-wei), July 11–13, 2022 | CacheSack chooses admission policies by workload categories using a knapsack formulation and deployment cost metrics. | A local runtime needs its own categories and measured costs; Google's flash-cache objective is not a laptop-runtime objective. |
| R7 | [Wong et al., Baleen](https://www.usenix.org/conference/fast24/presentation/wong), February 27–29, 2024 | Cache-residency episodes and an end-to-end backend-load metric guide admission and prefetching under a write-rate constraint. | This motivates measuring residency-limited useful work, not importing an ML model or flash-wear metric into Suffice. |
| R8 | [Xia et al., Learning-Augmented Heuristics](https://www.usenix.org/conference/osdi26/presentation/xia), July 13–15, 2026 | LAH separates a simple deterministic data path from occasional cache-level parameter learning; S4-FIFO is its evaluated instance. | Its learned objective is miss-ratio improvement. A Suffice controller must target whole-service cost and resource limits, and its training/overhead would need separate validation. |

R8 is a current 2026 research direction, not evidence that learning is the next implementation priority. The strongest common lesson is to make the expensive path simple, define the objective correctly, and account for the cost of the policy itself.

## 2. Separate four kinds of retained state

The current plan cache retains a compiled query and mutable worker together. A complete-result hit can replay output without preparing or decoding through that worker. Consequently, counting every repeated query as evidence for keeping its worker overvalues workers whose requests are already served by result hits.

The useful conceptual decomposition is:

| State | Why it exists | Relevant future event | Appropriate invalidation |
|---|---|---|---|
| Immutable generation | Establishes exact source/version meaning | Any valid read through an owner | Released only when all relevant owners expire |
| Compiled query | Avoids repeating source analysis/lowering | A computed execution of the same query and generation | Project retirement or explicit plan eviction |
| Mutable worker | Reuses prepared execution/decoder storage | A computed execution needing that worker | Independent byte/count pressure; release must not invalidate a cached result |
| Complete result | Avoids recomputation/decompression | A matching generation/source/range result lookup | Generation retirement, result pressure, or explicit clearing |

This is a design separation, not an assertion that all these allocations can already be detached cheaply. The implementation must inspect Query/Worker ownership before splitting objects. A bounded combined plan-and-worker cache is a valid smaller first step; it sacrifices some compilation reuse while making retention controllable.

Correctness identity and economic history have different requirements. A returned result must still match an exact key. Approximate frequency history may use bounded fingerprints because a collision would affect only an admission decision, provided it never authorizes replay or substitutes for exact key equality.

## 3. Derive the objective before choosing an algorithm

### 3.1 Primary measured objective

For a fixed request sequence and byte-identical outputs, minimize complete service time subject to declared retention limits:

$$
\min_\pi T_{\mathrm{service}}(\pi)
\quad\text{subject to}\quad
W_\pi(t)\le B_W,\quad R_\pi(t)\le B_R,\quad H_\pi(t)\le B_H.
$$

Here W, R, and H denote the declared worker-component, result-capacity, and policy-history pools. The implementation must define whether the inequalities hold continuously or at settled operation boundaries. Peak RSS and transient allocation are separate measurements. Report the service-time/retention trade-off across capacities rather than inventing one universal conversion between a byte and a millisecond.

### 3.2 Build and retain are two different decisions

Let D be the cost of computing and delivering a particular result without a retained complete result, R its cost when replayed to the same kind of consumer, and C the additional construction work still required. Let H be the number of useful future hits before eviction or invalidation. With fixed costs, creating the result is worthwhile only if

$$
H(D-R)\ge C+O+L(V),
$$

where O is incremental policy cost and L(V) is the lost future benefit of the complete victim set V. This is a conditional decision model, not a measured estimator or a guarantee. H and L(V) are unknown online and depend on the policy's own future decisions.

If there is no contention, no invalidation, and no incremental policy overhead, Populate-on-first-use has total cost

$$
D+C+(K-1)R,
$$

so for D>R its break-even request count is

$$
K\ge 1+\left\lceil\frac{C}{D-R}\right\rceil.
$$

An illustrative case with C=4 ms and D-R=0.2 ms requires at least 21 total requests. These numbers are a worked example, not a Suffice measurement. If D≤R, no finite number of identical repetitions repays a strictly positive C under this model.

**After construction has already completed, C is sunk.** The keep-versus-discard comparison must use future benefits and opportunity costs, not subtract the same completed build cost again. At that moment, even a candidate that should never have been built can still be worth keeping if free space exists and future replay helps. Conversely, discarding it cannot recover construction time. This distinction rules out the superficially attractive but incorrect policy of building every candidate and then claiming a post-build score prevented unprofitable construction.

### 3.3 Worker value is conditional on result misses

Let P_i be the preparation work avoided by retaining worker i. Its expected future benefit is approximately

$$
V_i^W=\sum_{t>now}\Pr(\text{request needs computed execution }i\text{ at }t)\,P_{i,t}.
$$

The event includes a result-cache miss or bypass, current-generation eligibility, and continued worker residency. It is not simply the probability that query i is requested. Holding both a complete result and its worker can be reasonable for other ranges or explicit bypasses, but the two benefits cannot be counted independently for the same result hit.

The total retained allocation is likewise not the sum of arbitrary ownership-category statistics. If two structures share an allocation, reclaiming one owner may free no bytes. A future unified allocator must track an allocation-owner graph or another explicit deduplication model. Until then, preserve separate named pools and avoid an invented “total memory” sum.

### 3.4 Delivery creates a ceiling on compute-only improvements

If a fraction f of baseline time is unavoidable output delivery and unchanged work, eliminating all other work yields a conditional speedup ceiling of 1/f. The current study's 99.65% selected plaintext fraction is a byte-selectivity measure; it does **not** establish f. Measure the timing fraction separately. Similarly, a JSON/hex transport represents B payload bytes with 2B hex characters before framing, but reducing those characters does not by itself establish a proportional wall-time gain.

## 4. Small implementation with strong invariants

### 4.1 A worker-byte cap is a meaningful first increment

Add a separately configured retained-worker-component limit and expose the actual charge. A minimal implementation can continue evicting combined plan/worker entries by LRU until the byte limit, plan count, and source-text limit all fit. An oversized worker may execute transiently and then be released; it must not displace every useful entry merely to fail retention. Later splitting compiled plans from workers can preserve more compilation reuse.

Enforce the contract after worker construction **and after execution**, including exception paths: a worker may grow lazily when it encounters a larger body. A constructor-only check does not bound retained post-run capacities. A cleanup guard must not allocate or throw. If a cached worker cannot remain within the settled-state budget, detach it safely while preserving the active execution owner until the call ends.

Useful telemetry distinguishes `worker_prepared`, `worker_reused`, `worker_retained`, `worker_oversized`, and budget-triggered evictions. A legacy `plan_reused` bit alone becomes ambiguous if query compilation and worker preparation are separated. The exact field names are design proposals, not existing API promises.

### 4.2 Result construction should reuse completed work when safe

The 0.11 Populate path performs a successful ordinary execution followed by two extra construction passes. A future capture-during-output path can stage an exact complete result while the initial execution already has the bytes. It must retain the output limits, exact ordering, range identity, and transactional admission behavior.

The difficult part is failure semantics. A consumer exception still propagates and prevents publication of a newly partial result. Optional capture allocation failure should disable capture and let the already requested ordinary output continue, where the API's contract permits that. No victim is removed until the candidate is complete and the commit requires no further allocating preparation. Report copied bytes, allocation behavior, and entire service time; “one pass” alone is not a performance result.

### 4.3 A transparent admission baseline before learned policy

Introduce any adaptive mode separately so Bypass, Use, and Populate keep their current meanings. A useful first comparator is a bounded second-touch gate:

1. Keep a fixed-capacity, aged history of eligible completed requests.
2. On a first observed exact-key identity, compute without building a complete result.
3. On a later observation still represented in history, allow the normal bounded construction path.
4. Build the candidate privately; gather the full set of would-be victims before admission.
5. When using estimated benefit, compare the candidate with aggregate victim benefit; reject without destroying existing entries if the comparison fails.

This is a proposed baseline, **not an implementation of S3-FIFO or TinyLFU**. It deliberately makes second accesses miss when the first did not materialize a result. A two-access-only workload can make it strictly worse than immediate population; the test matrix must expose that case. History must have its own byte/count bound. Storing every 64 KiB source string in an unbounded history would recreate the original resource problem.

## 5. Correctness and accounting obligations

The next change should preserve or explicitly strengthen these invariants:

1. **Exact output:** policy changes never alter returned row IDs, paths, order, or complete bodies.
2. **Exact generation:** history and caches cannot authorize old bytes for a new generation.
3. **Retired owners:** executing a retired lease creates no new persistent owner chain.
4. **Hit independence:** result hits can succeed without retaining or recreating a mutable worker.
5. **Settled worker bound:** retained component charges fit the configured worker cap on every completed or exceptional API exit covered by the contract.
6. **Actual capacities:** accounting uses retained capacities/owned components, not only requested lengths.
7. **Candidate isolation:** optional construction or node-allocation failure preserves existing retained entries until commit.
8. **Consumer semantics:** consumer failures propagate; partial new candidates never become hits.
9. **No early victim loss:** victim selection is planning; removal occurs only after candidate completion and all fallible commit preparation.
10. **Bounded control state:** history, ghost entries, counters, and statistics cannot grow with an unbounded stream of distinct sources.
11. **Checked arithmetic:** capacity sums, counters, size estimates, and epoch arithmetic reject or saturate rather than wrap.
12. **Honest boundary:** component limits do not imply RSS, decoder-library allocation, external-consumer, or peak-construction limits.

A budget test should exercise the behavior that threatens the invariant, not merely assert that the implementation's counter equals itself. For example, independently sum known retained capacities, force lazy growth, and check the observable release/reprepare behavior after both success and callback failure.

## 6. Adversarial workload matrix

| Workload | Why a plausible policy fails | Required observation |
|---|---|---|
| Hot pair, long one-touch scan, hot pair | LRU scan pollution; unconditional construction pays for objects never reused | Full service cost, build count, retained hot entries, exact output |
| Every item accessed exactly twice | Second-touch construction occurs after the last useful access | Additional build work and absence of later hits remain visible |
| Cyclic working set slightly larger than capacity | LRU and simple probation can thrash continuously | Repreparations, admissions/evictions, capacity sweep |
| Sudden hot-set replacement | Lifetime frequency locks in yesterday's hot objects | Adaptation interval and new hot-set service cost |
| One large candidate displaces several small valuable entries | First-victim comparison ignores aggregate opportunity loss | Victim set and combined reclaimed capacities/benefits |
| Popular but cheap computation, rare expensive computation | Frequency alone values the wrong avoided work | Per-class service and construction cost |
| Empty results and tiny payloads | Payload-byte cap permits unbounded metadata/count growth | Entry count, key/history size, source-length limits |
| Frequent project updates with identical shapes | Dimension/source similarity permits stale reuse if identity is weakened | Byte comparison through old/new leases and history reset semantics |
| Complete-result hits after clearing all workers | Worker retention is incorrectly required for hits | Zero new worker preparation on successful result hits |
| A large body encountered after earlier small queries | Scratch grows after the initial budget check | Post-run and exceptional-exit retained charge |
| Slow or throwing consumers | Internal compute timing is mistaken for useful-service time; partial candidate leaks | Complete session timing and exception/cleanup behavior |
| Long source strings and unique query spam | Admission metadata exceeds the cache it was meant to protect | Fixed history bound and lookup/hash overhead |
| External snapshot aliases and retired leases | Removing a catalog entry is mistaken for physical reclamation | Live-owner charge until the last owner is actually released |
| Allocation failure while staging candidate or victim bookkeeping | Existing entries are evicted before the operation can commit | Prior hits and exact outputs remain available |

No single successful trace establishes robustness across this matrix. Each new retained-state mechanism should satisfy its relevant correctness cases; performance comparisons can then use a small, explicitly chosen subset.

## 7. Experiment design that can reject the idea

Freeze source identities and use the same original-byte oracle for the baseline and candidate. Preserve the 0.11 historical result instead of relabeling it. Compare, at minimum, the old policy, the new worker budget with result bypass, and the new budget with the existing Populate/Use behavior. If admission changes, add it as a separate arm so a construction optimization cannot be confused with a retention-policy gain.

Use identical inputs, output consumer, request traces, and process-lifetime boundaries. Alternate paired order. Report raw session wall time, explicit oracle time, service time, preparation/construction time, complete output bytes, and policy bookkeeping. State CPU affinity, filesystem-cache treatment, compiler/codec versions, and whether the CPU is shared. A profiler-only compute benchmark can explain a result, but it cannot replace the service experiment.

Sweep worker capacity around actual working-set boundaries, including zero retained workers, one-worker capacity, a hot-subset capacity, and enough for all measured workers. Report retained peak/terminal components and preparation counts together. A lower retained footprint with worse latency is a trade-off, not automatically an improvement. Worker budgeting can be useful even when no speedup occurs because it changes enforceable resource behavior.

For performance, requests within one process are correlated; process pairs remain the independent reporting unit under the chosen experiment. Use observed ranges and pair direction counts when the run count is small. Do not convert a small sample into a universal confidence claim. Include failure traces separately from timed runs so fault injection does not contaminate normal-performance comparisons.

## 8. Direction beyond this increment

The larger system can eventually treat immutable source generations, reusable compilation, mutable execution scratch, complete outputs, and on-disk artifacts as a graph of owned resources with different useful lifetimes. Its controller should choose materialization and retention under explicit budgets while correctness remains independent of prediction quality.

A reasonable subsequent sequence is: measure marginal saved work; introduce bounded cohort statistics; explore aggregate-victim admission; then compare periodic configuration changes against fixed baselines. LAH suggests how the control path could remain outside ordinary requests, while CacheSack and Baleen motivate choosing the actual system objective. None supplies a ready-made proof or model for this runtime. Before learning, record counterfactual uncertainty: a retained hit does not reveal how expensive a miss would have been at that same moment.

The highest-value near-term result is therefore a smaller, bounded retained footprint with preserved semantics and transparent latency cost. A later speedup claim needs a measured mechanism—less decoding, less copying, fewer preparations, or less transport—rather than a more elaborate policy name.
