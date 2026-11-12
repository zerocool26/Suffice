# Suffice 0.13 — Foundations, gaps and the next research program

**Status:** engineering analysis grounded in the completed 0.13 implementation and experiment. Statements marked “proposal” are not implemented features or measured results. `REPORT.md` contains the actual results; `research/transport_design.md` preserves the source-grounded preimplementation transport review.

## 1. The useful abstraction is an end-to-end observation

For an admitted immutable generation G and request q, define the observable answer as an ordered sequence:

`O(G,q) = [(row_i, UTF8_path_i, body_i)]`.

The implementation may prepare a predicate, acquire a retained Worker, compute selected rows, capture an answer, replay it, or encode it differently. A successful call must produce the same sequence and valid completion metadata under the selected contract. This separates semantic correctness from the machinery used to avoid repeated work. The 0.13 experiment checks the entire observation against original body copies, not only a digest or the runtime's feature representation. [R1, R2]

The extension across versions is therefore compositional. Generation identity determines what data a query means. Query identity determines the selection. Result-cache identity determines when an answer can be reused. Transport framing determines what bytes reach the consumer and where a response ends. Correctness of one layer does not prove correctness of the next. A cached correct result can still be truncated, decoded under the wrong sequence, or acknowledged with an invalid count. [R2–R5]

For a successful full-output request, a useful refinement statement is:

`Decode_T(Encode_T(O(G,q))) = O(G,q)`

for admitted values and selected transport T, together with a terminal response that reconciles the delivered sequence. The equality is a design obligation, not a machine-checked theorem. Current support consists of executable invariants, adversarial framing fixtures and original-byte integration oracles. A future formal model should isolate this statement before attempting to prove an entire runtime.

## 2. A completion protocol must represent partial progress

Suppose the consumer has received the first k valid rows before a write fails. It has observed a prefix of the intended result, not a completed result. Appending arbitrary “error JSON” after half a binary frame cannot restore synchronization because the receiver has no reliable boundary at which to interpret it.

The 0.13 writer therefore has a monotone state rule: after any partial/failed output, future writes fail. The client accepts recoverable application errors only after their framing, sequence and delivered counts are validated. Malformed bytes terminate the connection. This limits the error state space rather than pretending that every failure is recoverable. [R3–R5]

A useful state-model proposal is:

`Idle → Receiving(q,k,b) → Completed(q)`

or

`Receiving(q,k,b) → ValidatedApplicationError(q,k,b)`

or

`Receiving(q,k,b) → FailedConnection`.

Only the first two terminal states permit another request. Here k and b are cumulative delivered rows and body bytes. Guards include matching sequence, strict row ordering, exact field lengths, requested row-range limits and output budgets. Native write poisoning prevents a transition from FailedConnection back into an apparently valid response stream.

The subtle application consequence is that streaming callbacks are not transactional. A callback can act on a valid prefix before a later exception or malformed terminal. “Exactly once” external effects are not provided by this pipe protocol. A future retry API must require an idempotent consumer, stage effects until completion, or expose a stable operation identity and application-specific reconciliation. Automatic replay would otherwise change semantics.

## 3. Four distinct bounds are needed

The word “bounded” needs a named quantity. Current code enforces wire-frame/line limits and request output caps. Existing runtime accounting limits named retained components. None is by itself a bound on total process memory or elapsed time. [R1, R3–R5]

| Bound | What it controls | What it does not prove |
|---|---|---|
| Frame/line extent | Maximum accepted unit of encoded input | Total output retained by a caller |
| Aggregate rows/body bytes | Total admitted delivery for one request | Native/Python allocator capacity or copies |
| Retained component budgets | Accounted snapshots, metadata, Workers, results | Whole-process RSS/private bytes |
| Deadline/cancellation | Maximum permitted waiting interval under a cleanup contract | Implemented in 0.13: no general deadline yet |

A memory analysis proposal should track at least native retained capacity, native transient encoding/decode buffers, OS pipe buffering, Python parser/transient objects and consumer-retained rows. With `collect=False`, the client avoids retaining a list of all result bodies, but a callback can retain anything it receives. The ownership boundary must be explicit. Neither the application's behavior nor allocator slack disappears because the parser enforces a body-byte cap.

A liveness contract proposal must specify a monotonic absolute deadline spanning the entire request. Resetting a timeout after every arriving byte can allow a peer to keep the request alive indefinitely. Native Windows and POSIX pipe I/O need different mechanisms; design from supported platform primitives rather than adding an unverified polling loop. Also define bounded stderr draining, cancellation after completion races, termination escalation and whether a cleanup deadline applies to child exit. These are protocol-level acceptance criteria, not optional UI polish. Python documents stream and subprocess behaviors, while Microsoft documents Windows CRT translation constraints. [S1–S3]

## 4. Why the measured improvement is plausible, and where it ends

For one returned row with P path bytes and B body bytes, binary response traffic is `48 + P + B`. JSONL carries an envelope and approximately `2P + 2B` hex characters. The precise JSON expression is evaluated independently in the harness. For large bodies and modest control traffic, removing hex therefore approaches a halving of encoded row bytes. With no rows, no such saving exists, and the binary terminal header costs more than an LF. The observed positive and negative cases agree with this representation-level prediction. [R1, R2]

It does not follow that time must improve by exactly twofold. Formatting, decoding, Python object creation, scheduler behavior and blocking all contribute. Nor does a large query-only ratio imply the same whole-session ratio: startup and Full admission remain useful, paid work. A model for future experiments can decompose `T_total = T_start + T_admit + T_query + T_deliver + T_finish`, but the present overlapping clocks do not identify these as independent causal components. A controlled ablation is needed for each claimed mechanism.

An unavoidable constraint remains: if every selected body byte must reach a new consumer, complete delivery has work proportional to the total bytes delivered, under a byte-processing cost model. Caching can remove computation and decoding; framing can remove expansion; neither makes full transfer independent of output size. To go further, shared ownership, previously known content or a different output contract must enter the model. The key research question becomes which work the caller actually needs, with the changed contract disclosed and validated.

## 5. Candidate extensions and their proof obligations

### 5.1 Request deadlines and cancellation — first priority

**Proposal:** define a request state machine with absolute deadlines and bounded shutdown. Admit requests only while Idle; cancellation and completion compete for one terminal transition; no further responses from a canceled sequence can be mistaken for the next sequence.

**Acceptance evidence:** live child that stops before header, midway through header, midway through body and before terminal; stderr filling independently; consumer callback failure; shutdown racing a valid terminal; one-byte-at-a-time progress; process exit without QUIT. Run native Windows and Linux cases. Verify both maximum wait and absence of surviving owned children. The current real-child EOF probes cover only a subset and must not be relabeled as this complete gate. [R4, R5]

### 5.2 Target-machine resource accounting

**Proposal:** record per-process and combined native/Python working set/private-byte observations alongside retained-component counters, output mode and process lifetime. For disk, separate immutable source/store bytes, active builds, content-addressed compiler cache, retained releases and disposable evidence fixtures.

**Acceptance evidence:** idle baseline; warmed twelve-project sessions; result eviction; Worker clear; repeated generation changes; streaming versus collected output; consumer retaining bodies; slow drain. Use the actual intended Windows laptop and report measurement method and sampling limits. Compare equivalent useful work and keep raw traces. A graph of logical Worker bytes alone would not answer the user's total-memory question.

### 5.3 Compressed body frames

**Proposal:** negotiate an explicit later protocol version with raw length, encoded length, codec identity and decompression cap. Compress only when predicted saved transfer/parse cost exceeds codec cost; preserve raw fallback.

**Proof obligations:** lengths checked before allocation; bounded decompression; exact bytes after decoding; codec version provenance; partial-write failure behavior; stable resource accounting. **Experiment:** tiny versus large bodies, compressible source versus entropy-rich data, cached versus computed rows and slow versus fast consumers. A wire-byte win alone is insufficient.

### 5.4 Shared memory or mapped immutable bodies

**Proposal:** let a consumer borrow an immutable region using a generation-bound descriptor and explicit lifetime rules.

**Proof obligations:** region cannot be reclaimed while borrowed; offsets and extents cannot escape the mapping; generation identity prevents stale reuse; process death releases ownership; platform handle permissions and inheritance are scoped. Measure mapping/setup and page-fault costs as well as steady transfer. Python materialization remains a real cost if the application ultimately requires an independent bytes object. Apache Arrow IPC is useful prior art for structured metadata and body framing, but importing its ecosystem does not automatically solve Suffice's ownership problem. [S4]

### 5.5 Content references and deltas

**Proposal:** provide a distinct API for clients that already possess verified content. Replies may contain generation-bound content references or validated deltas, followed by missing-body fetches.

**Proof obligations:** client possession is established; references name the right version; reconstruction is exact; stale generation transitions are detected; failed fetch leaves an explicit incomplete result. Compare end-to-end reconstruction and useful application completion, including cache misses. Do not compare handle-only replies with complete bytes and call the ratio a transport-only improvement.

### 5.6 Adaptive policy

**Proposal:** select population and retention policies from observed reuse, recompute cost, result size, mutation rate and memory pressure, subject to a fixed semantic contract.

**Proof obligations:** policy does not alter answers; bounded metadata; learning does not evade resource limits; cold-start and adversarial churn remain visible. Evaluate against static policies on held-out traces. Avoid making the same trace both the tuning objective and the evidence for generalized benefit. Earlier versioned retention experiments provide a starting point, not proof of the adaptive proposal.

## 6. Scientific gaps that remain

The current corpus has strong byte-level reproducibility but limited ecological coverage. Twelve round-robin partitions do not model independent repository dependency structures, mixed build tools or real editing behavior. Predicates based on body length are useful controlled tests but do not cover the whole intended query language or a developer agent's task distribution. [R1, R2]

There are only ten timing blocks per scenario on one shared pinned CPU. Balanced order reduces an obvious source of bias; it does not remove thermal, scheduler, cache or observer effects. The independent review preserves outliers and reports order sensitivity. Broader inference requires a predeclared sampling frame, independent workloads and machines, and a statistical analysis suited to the actual experimental unit. [R6]

The build evidence verifies current project inputs, binaries and explicit dependencies, but not every implicit system header/library in a hermetic environment. Core support archives were reused under verified identities, with predecessor compilation logs preserved. A future reproducible toolchain image could improve environmental closure; current file hashes must not be advertised as a complete reproducible-build proof. [R7]

The project also has no new Gemma-agent execution evidence from this transport milestone. Faster runtime service is a component result. Connecting it to agent task success requires actual model/tool traces, a task benchmark, baseline agent configuration, complete token/time/cost accounting and the competition's applicable rules. The preserved paper cannot substitute for these experiments.

## 7. Prioritized research sequence

1. Execute the supplied native Windows correctness path and fix any target-specific issue before publishing Windows behavior.
2. Implement the deadline/cancellation/stderr contract with adversarial liveness gates on both platforms.
3. Measure combined process memory and disk ownership on realistic multi-project sessions.
4. Add independent repositories and mutation/consumer-speed traces; keep full-byte oracles and balanced comparisons.
5. Evaluate compression or shared ownership only against the newly established workload bottleneck.
6. Integrate a real model-agent workload and measure useful task completion under the exact competition/application contract.

This order makes each new mechanism answer a measured limitation and gives it a concrete promotion gate. The present release completes the bounded complete-output transport milestone; the sequence above describes the next work, without labeling proposals as finished engineering.

## References

- **R1:** `REPORT.md` and `results/transport_v0_13/SUMMARY.json`.
- **R2:** `docs/transport_experiment_v0_13.md` and retained original-body oracle inputs.
- **R3:** `docs/binary_protocol_v0_13.md` and `apps/catalog/wire.hpp`.
- **R4:** `scripts/catalog_client.py`, `tests/test_catalog_framing.py`, `results/client_framing/RESULT.json`.
- **R5:** `research/transport_review.md`, native writer fixtures and integration receipts.
- **R6:** `research/results_review_v0_13.md`.
- **R7:** `research/release_validation_review_v0_13.md` and the two build manifests.
- **S1:** [Python 3.12 I/O documentation](https://docs.python.org/3.12/library/io.html).
- **S2:** [Python 3.12 subprocess documentation](https://docs.python.org/3.12/library/subprocess.html).
- **S3:** [Microsoft CRT `_setmode`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmode?view=msvc-170).
- **S4:** [Apache Arrow columnar format and IPC](https://arrow.apache.org/docs/format/Columnar.html#ipc-streaming-format).

External references provide platform/interface constraints and prior art. Proposed mechanisms and acceptance criteria are engineering deductions, not performance claims borrowed from those sources.
