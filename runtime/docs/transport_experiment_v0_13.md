# Complete-output transport experiment — Suffice 0.13

## Question and comparison

Does bounded SFC1 binary response framing improve complete useful service when the runtime returns the same ordered row IDs, UTF-8 paths and original body bytes as JSONL/hex? How much response traffic does it eliminate, and what happens when a request returns no body at all?

This protocol is written before the measured run. Actual execution status and results belong in `results/transport_v0_13/` and the release report. The experiment compares two transports in one 0.13 binary and one Python client. It does not combine new timing with the historical 0.12 observations, measure a changed handle/delta contract, or attribute every timing difference to one isolated CPU instruction.

## Fixed semantics and runtime policy

Both conditions use the same source, native binary, original corpus, captured generations, predicates, request order, complete-output oracle and Python `{row, path, data}` materialization. The native ordered diagnostic FNV computation remains enabled in both. Request lines remain the existing bounded TAB/LF protocol. Explicit Populate uses Capture in both conditions.

The two variants are `jsonl` and `binary`. They differ only in the selected response transport. JSONL remains the application default. The binary format is specified in `binary_protocol_v0_13.md`: a 16-byte versioned frame header, a 32-byte fixed row prefix and raw path/body bytes, or framed terminal JSON. Both transport paths use the same control-message assembly and client semantic validation.

Fixed native limits are 64 MiB logical snapshots, 8 MiB metadata, twelve project slots, 64 prepared plans, 32 Workers, 16 MiB retained Worker components and 64 result entries. Complete-result byte budgets vary by scenario as below. These quantities are component contracts, not RSS.

## Corpus and independent oracle

Reuse the exact 103 predecessor source files and twelve captured project stores from the preserved 0.11 corpus carried by the completed 0.12 evidence bundle. The original files contain 1,460,207 plaintext bytes. There is no recapture, re-encoding or new corpus selection during this experiment. The twelve projects are deterministic round-robin partitions of one source tree, not independently sampled repositories.

Preparation verifies all 187 retained input identities, original source-body digests, original suite/run linkage and the completed build's source/binary identity. It copies those inputs into the new result directory. The completed result bundle can itself serve as the input source for another new run; old absolute workspace paths are provenance, not required audit inputs.

The oracle reads original body copies, sorts original relative paths by UTF-8 bytes within each project, assigns generation-local row IDs and evaluates the requested body-length predicate directly. It does not use the runtime's derived packed features. Every measured request compares the complete row/order/path/body output, then retains row descriptors and body hashes. Empty selection is also a complete output and counts as one correctness check.

## Five scenarios

Let projects be numbered 0 through 11. Each process starts empty and fully admits all twelve generations before the trace.

| Scenario | Requests | Policy | Trace and purpose | Result budget |
|---|---:|---|---|---:|
| `reuse` | 48 | Populate | Four cycles through twelve projects, body length > 1,024; result replay with full delivery | 1,457,164 B |
| `pressure` | 26 | Populate | `[0,1]` four times, projects 2–11, `[0,1]` four times; scan displacement and reconstruction | 283,059 B |
| `mixed_queries` | 72 | Populate | Thresholds 1,024, 8,192 and 65,536 across all projects, repeated; varying output selectivity | 2,955,422 B |
| `computed` | 48 | Bypass | Same project order and >1,024 predicate as reuse; all answers computed | 0 B |
| `empty` | 48 | Bypass | Same project order with body length >1,073,741,824; zero emitted rows, control overhead visible | 0 B |

Budgets for Populate are derived independently as selected body bytes plus 24 bytes per result-row descriptor. The empty-control threshold exceeds every input body and is independently evaluated; it does not replace the query with a different special opcode.

The independent LRU model checks hits, admissions, whole victim sets, occupancy and cumulative native counters under these immutable-generation/full-range traces. Resource checks run after startup, every admission and every request. Worker peaks are maxima of those observed retained states, not allocation-profiler or process-memory peaks. Both transports must finish each matched process pair with identical native policy counters and resource state.

## Balanced schedule and observation unit

The default is **ten repetitions**, giving **100 fresh native processes and 4,840 complete-output checks** if successful. Each repetition runs all five scenarios and both transports. Within a scenario, order alternates JSONL/Binary and Binary/JSONL. Scenario order rotates by one position each repetition. A multiple of ten gives every scenario each position equally often under both transport orders.

Every comparison is paired by scenario and repetition before aggregation. Ten paired process blocks are the observations for a timing contrast. Requests within one process share state and are not independent performance replications. No failed observation is silently discarded or replaced. A failed scheduled run records its failure and requires a separately identified new experiment after correction.

Runner and child are pinned to the same permitted logical CPU; the original runner affinity is restored in a `finally` path. The CPU is shared, not exclusively reserved, and filesystem caches are not flushed. Run mode requires the corresponding Linux CPU-affinity APIs. The retained-record audit uses portable Python standard-library code.

## Complete service timing

The primary metric is:

`service_ms = raw_session_wall_ms - explicitly_timed_oracle_audit_ms`.

Raw wall spans native process creation through successful close. It includes readiness, twelve Full admissions, query and Worker acquisition, predicate execution, payload decode, Capture, native FNV, complete row/control transmission, Python validation/materialization, statistics requests, remaining bookkeeping and shutdown. Each request returns complete bodies into the same Python representation in both arms.

The subtracted intervals explicitly include full-output equality, retained descriptors/digests, independent LRU/resource checks and independent row-wire accounting performed during the measured session. Preparation, input copying, source freezing, oracle loading and identity checks outside the session are excluded. Raw records are serialized after close. Post-close consistency checks occur after the raw clock stops and are not subtracted again.

Subtraction does not undo the observer's effects on caches, allocation or scheduling. Raw-wall comparisons are retained as a sensitivity check. Native phase clocks overlap client clocks and are not additive isolated causes. A secondary sum of complete query-request times excludes startup, admissions, statistics and shutdown; it must not replace or be mislabeled as the primary complete-service result.

## Wire accounting

The Python client counts bytes actually consumed from native stdout and successfully written to native stdin. Per-request results partition response bytes into row and terminal-control bytes. Cumulative counters include readiness, OPEN, STATS, RUN and QUIT. The final close result is retained so every cumulative byte counter can be reconciled against the complete ordered response inventory.

The independent expected binary row size is:

`16-byte frame header + 32-byte row prefix + UTF-8 path bytes + body bytes`.

The expected JSONL row size is computed from the exact row envelope, sequence/row decimal integers, hex path, two characters per body byte and terminating LF. Every measured row-traffic counter must equal its independently calculated size. Control-byte counters describe the actual consumed terminal encoding, including framing. They are retained observations, not an independent network packet capture.

Report full-session response bytes and their row/control components. Request bytes should match between transports for paired traces. A reduction in encoded pipe bytes is not a measured reduction in physical I/O, copying, allocations, RSS or elapsed time. Binary framing is not compression and this client still materializes Python byte strings.

For large bodies, removing hex approaches a halving of the path/body component. Complete-session ratios can be much smaller when admissions and control traffic dominate. Empty responses can require more wire bytes in binary mode because a 16-byte header replaces one LF. Keep that outcome even if it is unfavorable.

## Analysis and retained evidence

For each scenario, divide JSONL by Binary within each repetition. Ratios above one favor Binary for service, raw wall or response bytes. Report the geometric mean, paired median, observed minimum/maximum and direction count. Convert a paired-geometric speed ratio g into an equivalent service reduction using `1 - 1/g`, not `g - 1`. Separate policy medians provide time scale but do not recover the paired estimator.

Report per-block wire reductions `1 - binary_bytes/jsonl_bytes`, retaining negative values. Do not pool requests from unlike scenarios into an invented universal speedup. Observed ranges are not confidence intervals. This study supplies descriptive local observations without population-level significance or production-wide generalization.

The result directory retains the source corpus/stores, original provenance suite/run, every compiled project input, Python driver/client, this method and the wire-format document, the measured build manifest, schedule, per-process records, raw wall/audit/phase values, wire counters and native policy state. A portable audit checks file identities, the independent original-byte oracle descriptors, schedule, result model, resource bounds, row-wire formula, cumulative wire reconciliation and paired arithmetic.

That audit validates retained records. It does not re-execute historical native outputs or recreate original timings. Fresh measurements require a fresh passed build and a new output directory.

## Promotion boundary

Keep JSONL as the compatibility default in this release. Binary is an explicit option until broader repository/workload evidence and target-machine validation support changing it. Native Windows binary-mode code and its correctness runner are supplied; Linux observations cannot establish Windows compiler compatibility, performance or memory behavior.

The experiment does not include model inference, developer-task success, cross-generation result reuse, shared-memory ownership, compression, handles, deltas, concurrent scheduling or cancellation deadlines. Those mechanisms require their own contracts and comparisons.
