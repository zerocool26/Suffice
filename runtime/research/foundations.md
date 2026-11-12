# Toward a bounded, demand-driven developer-tool runtime

Research date: 2026-11-11, America/Chicago. Baseline inspected: Suffice 0.11.0 and the preserved 0.9/0.10 architectural work. This is a design extension with explicit proof obligations and experiments. It does not claim that the proposed dependency graph, asynchronous scheduler, content handles, or incremental capture are implemented in 0.12. The release implementation and its new measurements must be reported separately.

## 1. What the project should become

The strongest larger direction is a runtime that decides **which exact information and execution state are worth keeping for the computations a developer actually demands**. It should coordinate ten projects as one resource-constrained workload while preserving each project's version and output contracts.

The earlier work already separates semantic sufficiency from physical speed, provides bounded continuation-encoding checks, and demonstrates an exact gate-to-payload boundary. Version 0.11 adds immutable generations, publication revisions, leases, capture, and a persistent service. The next architectural contribution is to connect these pieces without allowing a policy decision to weaken correctness.

That requires three independent questions for every reusable object:

1. **Validity:** Is this still an exact answer to this observation under these dependencies?
2. **Representation:** In what form can that answer be retained or reconstructed?
3. **Economics:** Is retaining that representation better than rebuilding it under the current demand and resource constraints?

Answering the third question cannot establish the first. A hot result may be stale; a cold result may be valid; a compressed representation may be exact yet more expensive to use. The architecture should make these distinctions mechanically visible.

The relevant research precedent is substantial. Adapton separates changes from demanded recomputation; Nominal Adapton makes reusable computation names explicit; self-adjusting computation studies equivalence with execution from scratch. These are precedents and correctness targets, not evidence that Suffice already inherits their proofs or performance. [F01–F03]

## 2. A layered identity model

One cache key should not stand in for all kinds of identity. Introduce the following conceptual records; the exact serialization is future API work.

| Record | Meaning | Must not be confused with |
|---|---|---|
| `BlobIdentity` | Exact byte-string identity, with length and algorithm/version domain | Filename, file ID, row number, or semantic meaning |
| `ManifestIdentity` | Ordered path-to-blob mapping and extraction/schema contract for one captured version | A coherent application commit unless capture establishes one |
| `TaskIdentity` | Operator implementation/version, declared parameters, dependency-resolution rules, and observable output contract | A particular native executor or its current performance |
| `DependencyWitness` | The observations made during one successful deterministic evaluation | Merely the positive file reads that happened to be logged |
| `RepresentationIdentity` | Encoding, physical layout, ABI/ISA requirements, and reconstruction contract | The computation's logical answer |
| `DemandLease` | A consumer's interest in a particular version/answer and retained owners | Permission to change that answer when a newer version appears |

Domain-separated hashes are useful compact indexes. For exact byte identity, either compare candidate bytes on a hash match or explicitly state the cryptographic collision-resistance assumption. A finite digest is not an injective mathematical encoding of arbitrary byte strings. The current SHA-256 checks are practical integrity checks; they do not prove semantic equivalence of distinct programs.

Stable names help recover sharing across executions, but a name is not permission to ignore a changed computation. Nominal Adapton provides a relevant semantic treatment of names and reuse. For Suffice, keep the stable lookup name separate from the implementation/environment identity that validates the located object. [F03]

### An important relocation counterexample

Suppose an unchanged file has row 8 in generation A. Inserting a lexically earlier path makes it row 9 in generation B. Its body can share a blob, and a per-blob pure feature can share a value. The previous `(row 8, path, body)` response cannot be replayed as a complete generation-B answer. Reuse must occur beneath the generation-local ordering boundary, followed by reconstruction of the new row/path sequence.

This suggests a useful first cross-generation optimization: share immutable body/feature values by exact content and extractor identity, while reconstructing generation metadata. Do not begin by transplanting old row masks or complete results across generations.

## 3. Dependency validity as an executable obligation

Build Systems à la Carte separates scheduling from rebuilding and discusses untracked inputs, volatile tasks, and nondeterminism. Its model makes a key requirement explicit: reuse depends on the actual task dependencies, including the rule/tool context. [F04] Build environments can also include tool versions, target platform, locale, paths, and selected environment variables; the dependency scope must be declared for the task rather than inferred from a source digest. [F07]

For a future deterministic task, expose observations through a versioned interface:

- `read_bytes(path)` returns an immutable content identity or a typed failure.
- `exists(path)` returns a versioned presence/absence value.
- `list_directory(path, policy)` returns a canonical membership observation.
- `read_config(key)` and `read_environment(key)` return explicit versioned values.
- `invoke_tool(tool_identity, arguments, inputs)` identifies all allowed external effects.

Untracked clock, network, random, filesystem, or process observations make the task volatile unless their values are explicitly captured as inputs. A tool whose dependency access cannot be intercepted or declared does not yet qualify for exact incremental reuse.

### Claim 1: equality of complete observations preserves deterministic output

Let an evaluation be deterministic except for calls to the observation interface. Assume its code, parameters, environment semantics, and initial internal state are fixed. Let its recorded sequence be `[(q1,a1), ..., (qn,an)]`. If replay against one immutable new world returns the same answer `ai` for every observation `qi` reached in the sequence, the task produces the same output and performs the same sequence of observations.

**Proof sketch:** before the first observation, deterministic execution is identical. Equal first answers imply identical state until the second observation. Induct on the observation number. A first control-flow divergence would require either different deterministic computation or a different earlier observation answer, contradicting the assumptions. The argument also covers dynamic dependencies: an unchanged controlling observation means the same next dependency is requested.

**Limits:** this is a theorem for the stated interface model, not a proof of a C++ implementation. The observations must be complete and resolved against one coherent immutable version. Digest equality is conditional on its identity contract. Undefined behavior, uncontrolled effects, and nontermination require separate handling.

### Counterexamples that define the boundary

1. **Absent configuration:** a task defaults to mode A because `config.json` is absent. A new file changes the answer although every previously read file has unchanged bytes. Recording only positive reads is unsound.
2. **Directory enumeration:** `*.cpp` gains one file. Rechecking old file digests does not validate the old file list.
3. **Extractor change:** the bytes are unchanged but an extension dictionary changes. Old feature values are not valid under the new extractor.
4. **Mixed capture:** task observations are individually valid but drawn from different application revisions. Their conjunction need not describe any actual committed state.
5. **Hidden compiler context:** a compiler binary, flags, target, or environment changes. Source identity alone does not validate old output.

The initial implementation should reject dependency cycles with a concrete cycle witness. Supporting cycles requires a separate semantic choice—such as a monotone fixed point with a convergence contract—not merely removing the cycle check.

### Early cutoff and trace budgets

After an input changes, reevaluate only demanded descendants. If a node's canonical observed value is unchanged, consumers depending only on that value need not be recomputed. This is a value-equality argument, not a universal property of equal file sizes or equal hashes of partial metadata.

The dependency graph itself consumes memory. Bound node count, edge count, key bytes, trace bytes, and retained observations. If a trace exceeds its budget, finish the task under a nonmemoized execution mode and discard its incomplete reuse record; do not store an incomplete trace as valid. Adapton supplies the demand-driven precedent. Salsa provides current practical examples of query eviction, dependency-based validation shortcuts, and cancellation, but its policies and Rust invariants cannot simply be imported into Suffice's C++ ownership model. [F01, F05, F06]

## 4. Representations form a graph, not a single cache tier

For one observation, candidate retained states might include:

- Immutable compressed bodies plus coarse reusable summaries.
- A prepared query with no retained worker scratch.
- An exact selected-row set tied to a manifest and query.
- Complete decoded output for that observation.
- A checked generated execution kernel.

Each representation needs a reconstruction edge with a precondition, output identity, cost, and temporary-resource bound. These candidates are not totally ordered. A prepared kernel may accelerate many future queries but not preserve a particular result; a row set can avoid predicates but still requires decoding; full output can avoid decoding while retaining far more bytes.

Do not use one opaque quality score to combine latency, memory, storage, and energy. A practical controller can minimize measured useful-session latency subject to explicit resource constraints, then report other costs separately. If the user supplies priorities, weighted objectives become meaningful, but the units and weights must remain visible.

### Claim 2: delivery is a lower bound under an unchanged body-output contract

For a consumer that must receive B arbitrary bytes through a channel of effective rate beta, delivery requires at least approximately `B / beta` time in the bandwidth-limited model, apart from protocol and synchronization. A predicate optimization does not remove those bytes. More generally, if an unmodified common stage consumes fraction f of a baseline session, eliminating all other work gives an ideal speedup no greater than `1 / f` under the additive model.

**Consequence:** a 10× end-to-end target requires reducing every unchanged stage whose combined fraction exceeds 10%, or changing what the consumer actually needs. These are planning bounds, not a measured percentage breakdown of 0.11.

The current protocol transports bodies as hexadecimal in JSON. Encoding B body bytes as hex requires 2B payload characters before the surrounding syntax. A binary framed transport may reduce that representation overhead while returning the same bytes, but it still must be measured end to end. It is not automatically a twofold service speedup.

Content handles, aggregates, and deltas are stronger possibilities only when the consumer accepts their contracts:

| Contract | Exactness obligation |
|---|---|
| Full bodies | Deliver the complete ordered row/path/body sequence |
| Content handles | Retain or resolve the identified immutable bodies for the lease lifetime |
| Aggregate | Return the exact declared aggregate; callers cannot infer omitted bodies |
| Delta | Name the required base manifest, ordered patch operations, and final identity |

A delta against the wrong base must fail. A handle is not a byte transfer until its downstream fetch is charged. Calling either one equivalent to full-body service without counting consumption would create a false improvement.

## 5. Shared ownership makes memory accounting a set problem

Let U be the set of separately owned allocations, with charged sizes `b(u) >= 0`. For roots S, let `closure(S)` be all allocations kept live by those roots. Then the logical retained footprint is

`C(S) = sum(b(u) for u in closure(S))`.

This is not generally the sum of object-local sizes. If A and B both retain a 1 MiB snapshot and have 1 KiB private metadata each, evicting A reclaims only its 1 KiB while B remains. Evicting both can release the shared MiB. A fixed per-entry charge of either 1 MiB or 0.5 MiB misstates one of these transitions.

### Claim 3: retained-allocation cost has diminishing marginal additions

For a fixed ownership graph with root sets `S subset T`, the additional allocations exposed by adding root x to S include those exposed by adding x to T. Thus `C(S union {x}) - C(S) >= C(T union {x}) - C(T)`. This follows directly from set union and nonnegative sizes.

The useful eviction quantity is instead `C(S) - C(S minus V)` for a proposed victim set V. It depends on the complete surviving ownership graph, including external leases. This derivation explains why static value-per-byte rankings can make poor decisions even with perfect value estimates. It does not prove that an online global optimum is cheaply attainable.

A tractable first controller should group accounting by actual shared owners, report private and shared charges separately, and recompute reclaimable bytes for each small candidate victim set. Keep policy history bounded. Evicting a logical entry that is externally pinned can remove its future discoverability without immediately freeing its storage; those are different events.

### Claim 4: an admitted-memory bound needs reservations before work starts

For allocations controlled by a budgeted allocator, maintain

`live_controlled_bytes + outstanding_reservations <= B`.

Admit a job only after reserving a proven upper bound for its additional controlled allocations. During execution, atomically convert reservation bytes into live charges; releasing an allocation restores its charge. No transition increases the sum beyond B if reservation increases are checked first. The invariant follows by induction over these transitions.

The proof fails if an uncharged allocation path exists, an estimate is treated as a bound, or a reservation is released while another task can still access the memory. Unknown-size output must grow by checked chunks, spill under a separate disk budget, stream under backpressure, or stop explicitly.

Even a correct controlled-allocation invariant is not an RSS bound. Allocator fragmentation, stacks, libraries, mappings, OS pages, and consumer processes require separate measurement or control. Report retained controlled bytes, active work reservations, active controlled bytes, and process RSS separately.

There is also a hard semantic limitation: arbitrary independently pinned immutable histories cannot all remain exactly accessible in bounded memory without external storage, recomputation, refusing further leases, or limiting history. Worst-case incompressible data defeats a universal compression promise. The API must choose among those mechanisms rather than claiming that an eviction policy can eliminate the tradeoff.

## 6. Demand, cancellation, and publication must share one lifecycle

Adopt a proposed execution record with `task identity`, `world/version`, `demand count`, `reservation`, `cancellation state`, `private candidate`, and `completion state`.

1. A demand acquires an immutable version lease and either joins an identical in-flight computation or creates one after reserving resources.
2. The computation builds privately. It may stream output only under a declared partial-output contract; an incomplete stream is not a successful cached result.
3. At safe boundaries it checks cancellation and available output/trace budget.
4. Before reusable publication, it validates the dependency witness and checks the required revision policy.
5. A successfully published object receives the live charge; unused reservations are returned.
6. Failure or cancellation releases private state only after all users of it have completed.

Joining work needs exact semantic/version identity. Similar-looking queries are insufficient. One consumer releasing its demand does not cancel shared work while another consumer still requires it. A latest-version request can reject a stale candidate; a snapshot request can still finish validly against its pinned older generation. Keep these modes explicit.

### Cancellation is a request, not reclamation

For a CPU loop, a cancellation check every bounded chunk only gives a useful latency bound if each chunk's cost is bounded. A large codec call, blocked callback, or arbitrary tool subprocess can invalidate the bound. The scheduler must distinguish cooperative checks, completed cancellation, timeout, and forced process termination.

On Windows, `CancelIoEx` marks outstanding operations for cancellation and does not wait for their completion. Associated `OVERLAPPED` storage cannot be freed or reused until completion is known. The operation can still finish normally. Therefore resource charges and owner lifetimes must follow completion, not the moment a cancellation request is issued. [F08]

A slow consumer needs a bounded queue and backpressure. Otherwise removing computation overhead merely moves unbounded memory growth into pending output. Useful admission controls include per-project concurrency limits, global reservation limits, and aging of queued demands. Priority and fairness should be evaluated under the ten-project use case rather than selected only for maximum single-job throughput.

## 7. Three experiments that can falsify this architecture

These are proposed follow-on experiments, not completed results. Each compares identical observations where claimed and retains cold, warm, changed, and adverse cases.

### E1 — Incremental validity with bounded dependency witnesses

**Question:** can cross-generation reuse reduce changed-session work without missing negative or structural dependencies?

Build a small developer-tool adapter that derives an index from a versioned source manifest. Compare full recomputation, whole-generation reuse, and dependency-validated reuse. Use controlled changes: irrelevant body edit, relevant edit, new matching file, absent-to-present config, rename, extractor change, same-value edit, and dependency switching. Include a query reading most of the project as a likely negative case.

**Oracle:** recompute every demanded output from scratch against the same immutable manifest and compare complete canonical bytes. Deliberately omit each dependency class once in a fault-injected reference to prove the test detects that missing class.

**Metrics:** complete session time, witness-validation time, recomputed nodes, trace bytes, retained bytes, and cold-start overhead. Vary changed fraction and demanded fraction separately. Require zero stale outputs and the trace budget to hold. If validation costs more than recomputation for a workload, preserve that result and use the simpler path there.

### E2 — Representation and downstream consumption

**Question:** is the next dominant improvement in computation, output representation, or consumer demand?

For exactly the same full-body output, compare the present JSON/hex protocol with a bounded binary frame protocol. Then run a separate contract experiment comparing full bodies, pinned handles, and base-checked deltas for a consumer that actually resolves or applies them. Vary selectivity, body entropy, body size, locality, repetitions, and consumer speed. Keep the exact predicate and decoded data fixed within each comparison.

**Oracle:** the final consumer-visible bytes and order must match after all handle resolution or delta application. Inject stale bases, expired leases, truncated frames, and cancellation. Never compare a handle-only producer timer with a full-body consumer timer.

**Metrics:** first usable answer, total producer-plus-consumer time, delivered wire bytes, decoded/copied bytes, allocation counts, retained/reserved bytes, and peak RSS. A binary transport improvement can be reported independently of a changed consumer contract. Reject an optimization that only shifts memory or work into an unmeasured consumer.

### E3 — Ten-project ownership and cancellation pressure

**Question:** can shared work and bounded reservations improve useful progress under project switching, stale work, and slow consumers?

Use ten project namespaces, alternating active roots, duplicate demands, captured updates, large cold scans, retained old leases, and consumers with controlled read rates. Compare serialized execution, a fixed concurrency pool, and reservation-aware admission with exact in-flight deduplication. Initially use a small predetermined set of job memory bounds rather than a learned predictor.

**Oracle:** every completed demand returns the exact output for its stated version; canceled/partial demands never enter the successful cache. Assert the controlled allocation/reservation invariant at every state transition. Track owners through delayed I/O completion and inject failures before/after candidate publication.

**Metrics:** median and tail completion latency, oldest-demand waiting time, useful completed work, wasted work after cancellation, reservation occupancy, retained old-generation bytes, physical RSS, and disk writes. Run on the target native Windows laptop as a separate platform gate; Linux-only results cannot establish the requested Windows behavior.

The hypothesis fails if coordination overhead dominates, old pinned generations block useful admission indefinitely without a declared policy, resource bounds are exceeded, or an apparently canceled operation still accesses released storage.

## 8. Prioritized engineering sequence

First remove unnecessary retained worker state while keeping current exact outputs and serialized behavior. That is an immediately measurable change with a smaller proof surface than a new dependency runtime. Preserve preparation costs in the comparison.

Next introduce a bounded binary output route or a narrowly scoped incremental feature reuse experiment, choosing according to measured complete-session cost. Do not mix transport, dependency tracking, worker changes, and cache policy into one unexplained performance result.

Only after those measurements introduce the general demand graph and concurrent admission protocol. Its graph, queue, and metadata are themselves budgeted resources. The long-term compiler can then choose valid representations and execution tiers using actual costs, with bounded solvers returning unknown rather than silently accepting unproved rewrites.

The ambitious deliverable is an exact, measurable resource governor for developer computation: it controls what is retained, when work becomes worthwhile, how dependencies establish validity, and when obsolete work stops consuming the laptop. It does not require a new OS before these benefits can be tested.

## Sources

- **F01:** Matthew A. Hammer, Yit Phang Khoo, Michael Hicks, Jeffrey S. Foster. *Adapton: Composable, Demand-Driven Incremental Computation*. PLDI 2014. DOI: 10.1145/2594291.2594324. [Author-hosted accepted paper](https://www.cs.umd.edu/~mwh/papers/adapton-submit.pdf); [artifact](https://www.cs.umd.edu/projects/PL/adapton/). The linked accepted manuscript carries a 2013 draft date; the publication is PLDI 2014.
- **F02:** Umut A. Acar, Matthias Blume, Jacob Donham. *A Consistent Semantics of Self-Adjusting Computation*. ESOP 2007; full version submitted 2011-06-02. [arXiv:1106.0478](https://arxiv.org/abs/1106.0478).
- **F03:** Matthew A. Hammer et al. *Incremental Computation with Names*. OOPSLA 2015; arXiv v6 revised 2021-03-23. DOI: 10.1145/2814270.2814305. [arXiv:1503.07792v6](https://arxiv.org/abs/1503.07792v6).
- **F04:** Andrey Mokhov, Neil Mitchell, Simon Peyton Jones. *Build Systems à la Carte: Theory and Practice*. Journal of Functional Programming 30, e11, 2020. DOI: 10.1017/S0956796820000088. [Author manuscript](https://simon.peytonjones.org/assets/pdfs/build-systems-jfp.pdf).
- **F05:** Salsa project. [Tuning: cache eviction and cancellation](https://salsa-rs.github.io/salsa/tuning.html). Current documentation accessed 2026-11-12 UTC; no source publication date asserted.
- **F06:** Salsa project. [Durability](https://salsa-rs.github.io/salsa/reference/durability.html). Current documentation accessed 2026-11-12 UTC. Here durability concerns change frequency/validation shortcuts, not storage durability after power loss.
- **F07:** Reproducible Builds project. [What's in a build environment?](https://reproducible-builds.org/docs/perimeter/). Current primary project documentation accessed 2026-11-12 UTC.
- **F08:** Microsoft. [CancelIoEx function](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex). Current primary platform documentation accessed 2026-11-12 UTC.

Claims 1–4 are explicitly stated reasoning for the proposed interface and accounting models. They are not presented as novel mathematical results, as formal verification of Suffice, or as theorems inherited wholesale from the sources. No paper PDF is redistributed by this document.
