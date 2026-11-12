# Research and mathematical architecture for the revised Suffice paper

Research date: 2026-11-12 UTC. This memo supports revision of the Kaggle paper; it is not a claim that a Gemma experiment has run. Only primary literature, author-hosted papers, conference proceedings, and official product documentation support the external claims below. The companion sources JSON records URLs, versions, provenance, and source-specific limits.

## 1. Recommended paper concept

**Suggested title: Suffice: Exact Repository Evidence for Local Coding Agents.**

The strongest defensible contribution is an executable systems resource for generation-bound repository observations, accompanied by complete-service evidence and explicit failure/ownership contracts. The revision also includes a native-tested read bridge; the complete Gemma host integration remains unevaluated. The runtime supports deterministic integer predicates over admitted artifact features and returns original bodies; it does not implement a semantic retriever, call-graph engine, learned context selector, or autonomous coding agent.

The earlier paper packet describes the 0.11 experiment: 24 processes, 888 complete-output checks, and mixed or adverse results from complete-result population. The 0.13 report adds a separate controlled transport experiment: 100 processes and 4,840 complete-output checks, with paired geometric JSONL/Binary complete-service ratios of 1.799–2.138 across four nonempty scenarios. Empty output remains mixed and sends approximately 2.30% more response bytes in binary. These experiments answer different questions. Do not pool them or describe their ratio as an improvement across runtime versions. [L1–L3]

**Paper-ready contribution paragraph**

> We present Suffice, a deterministic repository-artifact runtime that binds reusable query results to immutable generation ownership and exact request identity, accounts for named retained components, and preserves complete ordered output across two response transports. Our contribution is an inspectable implementation and evaluation boundary connecting artifact validity, ownership, and delivery. We evaluate the runtime independently of a language model and provide a native-tested read bridge with a Gemma host integration design. Neither the runtime experiment nor the bridge establishes improved issue resolution, context relevance, or token efficiency.

Use “generation-bound” in the title and abstract. “Generation-typed” can describe the semantic discipline in the formal section, provided the paper explains that runtime ownership and identity checks enforce it. No dependent type system or machine-checked implementation proof has been supplied.

## 2. Current Gemma 4 facts and the integration boundary

The current Google model card lists E2B, E4B, 12B, 26B A4B, and 31B variants, native function calling, a system role, and context windows of 128K or 256K depending on the variant. This is model-family documentation, not an experimental result for Suffice. [R1a]

Google's function-calling guide passes tool declarations through the processor's chat template. The application parses and validates requests, executes allowed functions, and returns responses; the model does not execute the function itself. The prompt-format documentation specifies dedicated tool declaration, call, and response delimiters. An adapter should use the supported model/template implementation, rather than assuming that every model output is arbitrary JSON. [R1b, R1c]

For any future experiment, record the exact permitted model artifact/revision, processor and tokenizer revisions, inference library, quantization, context limit, sampling settings, reasoning mode, hardware, and tool budget. Choose a model that the applicable evaluation harness permits. Model-family marketing or another paper's benchmark numbers cannot fill a missing Suffice measurement.

### Implemented read bridge and remaining adapter work

The new bridge exposes a small read-only evidence operation while preserving the application's existing edit/test controls. It accepts only a host-approved scope, exact generation token, bounded first-row/count, and body-byte budget. It checks host-approved generation/revision/rows through STATE, verifies PIN revision, collects RUNPIN output privately, and checks terminal generation/revision before emitting canonical UTF-8 or base64 JSON. Default retirement rejection and explicit historical access are native-tested. [L5]

The broader conceptual operation is:

    observe(project, generation_token, selection, output_budget) -> observation

This conceptual signature is not a new native command. The implemented bridge uses generation-local row ranges. A future generation-owned path-to-row index could map named candidates to those ranges. Any added path index, semantic ranking, line slicing, or prompt renderer needs its own version and provenance.

The bridge returns a generation token, ordered source identities, explicit row range, and completion disposition. It uses UTF-8 for valid text and tagged base64 otherwise; no invalid bytes are silently replaced. Future line-numbered excerpts must record their source body identity and exact range. “Complete output” describes the selected runtime result, not retrieval of every relevant file.

The current competition's prediction-track overview documents a restricted tool inventory and sandboxed skill execution. A proposed integration must fit the allowed harness route; do not present a newly invented top-level tool name as automatically permitted. The paper-track rules and prediction-track execution restrictions are separate documents. [S6]

### Proposed control sequence

1. Establish a coherent capture while repository mutation is paused; admit and pin generation G.
2. Run the chosen external retrieval policy against an index bound to G. Keep retrieval scores separate from validity checks.
3. Resolve candidate identities within G, request bounded complete artifacts, and validate native completion.
4. Render the declared model view. Record what was included, excerpted, or excluded.
5. After an edit, finish or abandon pending observations, recapture the changed tree, and admit G'. Do not reinterpret an old generation-local row ID in G'.
6. Permit historical reads only when explicitly marked historical. Require current-generation validation before using evidence to justify a current edit or test claim.

Step 2 and the edit barrier are new adapter work. Whole-artifact rendering in step 4 is implemented; a relevance-aware excerpt renderer is not. Existing immutable leases support historical consistency; they do not automatically follow changes in the live worktree. [L2, L3, L5]

## 3. Eight strongest reference groups and exact contrast paragraphs

The paragraphs in this section are proposed related-work text, not quotations from the cited sources. Their scope is deliberately narrower than a claim of first invention.

### R1. Google DeepMind, Gemma 4 official documentation, accessed 2026-11-12

**Contrast paragraph.** Gemma 4 provides native structured function calling, while its application remains responsible for validating and executing requested operations. Suffice addresses this application boundary: an accepted repository read should identify the generation it observes and deliver the selected artifact bytes completely. The present paper evaluates that deterministic backend; it does not evaluate Gemma's tool selection, reasoning, or patch quality. [R1a–R1c]

### R2. Yang et al., SWE-agent, NeurIPS 2024

**Contrast paragraph.** SWE-agent studies how an agent-computer interface affects repository navigation, editing, test execution, and software-task performance. Our interface contribution is narrower: it specifies the identity, completion, and retained ownership of a repository-tool result. Such guarantees may support an agent interface, but they cannot substitute for an issue-resolution evaluation or establish that the model uses the returned evidence effectively. [R2]

### R3. Wu et al., Repoformer, ICML 2024

**Contrast paragraph.** Repoformer learns when retrieval is useful for repository-level code completion, avoiding unnecessary context acquisition. Suffice separates that relevance decision from whether a requested artifact is valid for a particular generation and delivered exactly. A selective retriever could use this runtime, but the current predicates are deterministic artifact features, not a learned relevance policy. We therefore measure complete tool service rather than report retrieval-quality gains. [R3]

### R4. Weng et al., When Retrieval Hurts Code Completion, arXiv v1, 2026

**Contrast paragraph.** Weng et al. directly study obsolete repository snippets and show that temporal mismatch can induce incompatible completions in a small diagnostic dataset. We do not claim to discover stale-context harm. Suffice supplies a runtime-level identity contract that makes historical observations explicit; preventing their accidental use as current evidence additionally requires an edit/capture barrier. A pinned snapshot alone does not prove that its contents match the latest workspace. [R4]

This is directly relevant prior art and should survive page-budget cuts. Its current primary record describes a preprint submitted to a journal, not a confirmed accepted journal article.

### R5. Wang et al., SWE-Pruner, arXiv v4, 2026

**Contrast paragraph.** SWE-Pruner selects task-relevant source lines to reduce coding-agent context. Suffice's binary response path changes the representation of complete artifact bytes between native service and Python consumer. It does not prune model context. If both transports produce the same rendered prompt, token counts should remain identical. Learned pruning may be composed above this boundary, but it must be evaluated as a distinct transformation with its own quality and provenance obligations. [R5]

### R6. Mokhov, Mitchell, and Peyton Jones, Build Systems à la Carte, ICFP 2018

**Contrast paragraph.** Build Systems à la Carte separates scheduling from rebuilding and formalizes the dependency information needed to reason about build correctness. Suffice reuses exact queries within immutable generations; it does not infer all dependencies of arbitrary compilers, tests, or repository tools. Extending reuse across edits would require a sound dependency witness, including relevant negative and namespace observations, rather than matching only the bodies already read. [R6]

Use the 2018 conference citation consistently if citing the primary PDF listed below. The previous packet used the expanded 2020 journal version; these are related versions, not interchangeable bibliographic metadata.

### R7. Hammer et al., Adapton, PLDI 2014

**Contrast paragraph.** Adapton develops demand-driven incremental computation using a demanded computation graph and a separation between incremental computation and its observers. Suffice does not introduce demand-driven reuse or incremental dependency tracking. Its current contract is more restricted: immutable-generation query/result reuse with explicit owner accounting and complete delivery. An incremental extension must be compared against established change-propagation systems rather than claim that memoization or observer separation is new. [R7]

### R8. Bazel, Skyframe official documentation, accessed 2026-11-12

**Contrast paragraph.** Skyframe uses immutable values, explicit dependencies, and invalidation to support incremental evaluation; its documentation warns that unregistered input reads can invalidate correctness. This is a direct boundary on Suffice's future reuse claims. A call graph or embedding neighborhood is useful retrieval evidence but is not automatically a complete dependency graph for a computation. Environment, configuration, directory membership, and absence tests may also influence the answer. [R8]

### Useful supplemental references

RepoCoder provides an earlier iterative repository-retrieval baseline. Agentless supplies a simple localization/repair/validation alternative to elaborate autonomous control. ACE addresses evolving contextual strategies rather than byte-level repository identity. These are useful if the revised paper allocates space to the future agent experiment. [S1–S3]

The latest primary version of Evaluating AGENTS.md reports that repository context files do not generally improve task success, while a November 2026 preprint studies costs from stale instruction files. These motivate measuring actual tool behavior and completion, rather than assuming more context helps. Neither implies that snapshot identity repairs semantically outdated instructions: a perfectly current file can itself contain obsolete guidance. [S4, S5]

## 4. Formal model and conditional guarantees

This section is an analysis of the existing contract plus explicitly labeled proposals. The equalities are specification/proof obligations; current evidence is executable checking, not a mechanized proof of the whole runtime.

### 4.1 Repository, generation, query, and observation

Let a captured generation be an ordered finite sequence

\[
G=((p_0,b_0),\ldots,(p_{n-1},b_{n-1})),
\]

where p is a canonical relative UTF-8 path and b is an arbitrary byte string. Paths are unique and ordered by their encoded bytes. A generation owner g identifies this immutable object; it is not merely a shape, row count, or promise that the working tree has not changed.

Let f_v(p,b) be the versioned supported feature extractor. Native disk admission verifies the stored supported features against original bodies. Let q=(s,a,c) contain exact predicate-source bytes s and a global input-row range beginning at a with count c, or continuing to the end. The range is over generation rows, not over the kth matching result.

\[
\mathcal O(g,q)=
[(i,p_i,b_i)\mid i\in[a,\min(n,a+c)),\
       P_s(f_v(p_i,b_i))=\mathrm{true}],
\]

in increasing row order. Output budgets may cause explicit failure; they must not silently redefine the successful observation.

The implemented result identity is generation ownership, exact program bytes, and exact requested range. The supported extractor/runtime semantics are fixed by the admitted generation and executing implementation. A future cache shared across differing extractor implementations must additionally bind extractor and schema identities. Do not claim the current in-process key already implements a general cross-version semantic cache. [L1–L3]

### 4.2 Observation refinement

For admitted inputs, deterministic query semantics, a correct encoder/decoder, and successful complete execution:

\[
\operatorname{Decode}_{T}(
  \operatorname{Encode}_{T}(\mathcal O(g,q)))=\mathcal O(g,q),
\quad T\in\{\mathrm{JSONL},\mathrm{SFC1}\}.
\]

**Argument sketch.** The encoder serializes each row's identity, exact path extent, and body extent without altering the bytes. The decoder consumes the declared extents in order, validates framing and request constraints, and accepts completion only when terminal counts reconcile with received rows/body bytes. Induction over the rows gives preservation of the intended sequence for correctly produced frames. Cached replay must satisfy the same observation as computation, so representation and reuse can be tested against one oracle.

**Important limit.** Structural client validation does not independently authenticate an arbitrary well-formed body against the repository. A faulty or malicious native process could emit different bytes with valid lengths/counts. The experiment's independent original-byte oracle checks this semantic equality; the ordinary Python client does not reproduce that oracle for every request. FNV diagnostic output is not a cryptographic proof. The implemented service assumes trusted local components and storage within its documented admission model. [L2–L4]

### 4.3 Historical consistency and current freshness

A lease to g can remain valid after publication of g'. This is historical consistency:

\[
\operatorname{Read}(g,q,t_1)=\operatorname{Read}(g,q,t_2)
\]

when both reads succeed, even if the active generation changes between them. It does not establish that either read equals a query over the current worktree X(t).

For a proposed adapter, define current-evidence acceptance:

\[
\operatorname{AcceptCurrent}(e,t)
\Rightarrow
e.g=\operatorname{ActiveGeneration}(t)
\ \land\ 
\operatorname{CaptureMatches}(e.g,X(t)).
\]

The second conjunct needs an explicit quiescence/transaction or coordinated mutation contract. Comparing only a stored generation token does not notice an unobserved external edit. The current capture implementation checks individual files and inventories but does not establish an atomic filesystem-wide snapshot of a changing tree. [L1–L3]

### 4.4 Completion and prefix semantics

For intended response O, a failed request may expose a prefix O[0:k]. A complete validated native error can report that prefix and leave the command stream synchronized. Partial framing, write/flush failure, malformed output, or callback failure makes the connection unusable. The safe state transition is monotone: an output failure cannot become a later successful terminal on the same stream.

A streaming callback may already have acted on the prefix. Consequently:

\[
\text{exact row delivery}\ \not\Rightarrow\
\text{transactional or exactly-once external effects}.
\]

An agent adapter should stage context assembly until completion before passing it as a successful tool observation. Idempotent or staged side effects are needed before implementing retries. No general request deadline is implemented, so safety under observed failures does not imply liveness against a silent live child. [L2–L4]

### 4.5 Sound cross-generation reuse: future work

For a deterministic external computation F, let W contain all observations that can influence its result. A sufficient reuse condition would be:

\[
\operatorname{Complete}(W,F,X)
\land
\operatorname{Obs}_W(X)=\operatorname{Obs}_W(X')
\Rightarrow F(X)=F(X').
\]

Completeness is the difficult premise. W may need file contents, absence, directory listings, glob expansion, symlink resolution, configuration, toolchain and environment identity, and dynamically discovered dependencies. Determinism and discovery validity must also hold. Existing dependency-trace work and Skyframe motivate this obligation; the equation is a proposed specification for future Suffice work, not a theorem about its present implementation. [R6, R8]

In particular, a graph embedding, a set of previously retrieved snippets, or a content hash alone is not a witness that all observations influencing F were represented. Generation-local row positions can also shift after insertion; content-level reuse needs an explicit mapping.

### 4.6 Bounds and costs

Keep five quantities separate: per-frame bytes, per-request output, named retained components, actual live/RSS memory, and elapsed time. The current implementation controls the first three according to their individual contracts. Whole-process memory bounds and general request deadlines remain unimplemented. [L2–L4]

For a row with P path bytes and B body bytes, SFC1 uses 48+P+B encoded bytes: 16 frame-header bytes, 32 row-prefix bytes, and exact path/body bytes. JSONL hex encoding doubles the represented path/body characters before its envelope. No output rows means no such saving. This explains a representation opportunity, not a fixed time ratio or a token reduction. [L2, L3]

If a fresh consumer must receive L selected body bytes through a byte-oriented channel, full delivery has an Ω(L) byte-work lower bound. Reuse can eliminate recomputation; it cannot make complete independent materialization free. Shared ownership, handles, or deltas change the consumer contract and require their setup/resolution cost to be measured.

## 5. Mathematical objective and falsifiable hypotheses

### 5.1 Agent objective

For task distribution D, fixed permitted model m, and policy π, let Y indicate independently verified task resolution; T be complete episode time; N be input/output model tokens; and M(t) be actual controlled memory ownership over time. First compare task completion under fixed budgets:

\[
\max_{\pi}\Pr_{x\sim D}[Y_{m,\pi}(x)=1],
\quad T\le B_T,\ N\le B_N,
\]

subject to the proposed observation-validity contract. This is a research objective, not an implemented optimizer. A separate cost claim can test noninferiority in task completion with a predeclared margin δ and then compare T or N. Avoid optimizing a weighted score whose weights are chosen after seeing the results. Treat timeouts/failures according to a predeclared rule rather than discarding them.

For runtime retention, the expected value of admitting candidate e can be expressed as:

\[
V(e)=
\sum_k \Pr(H_{e,k})\,
  (C^{\mathrm{compute}}_{e,k}-C^{\mathrm{replay}}_{e,k})
-C^{\mathrm{admit}}_e-C^{\mathrm{validate}}_e
-C^{\mathrm{displacement}}_e .
\]

H means a valid hit occurs before eviction/invalidation. This accounting makes future benefit and displaced competitors explicit. It does not describe a policy implemented in 0.13, which uses its documented fixed policies and limits. Capture during useful computation can change admission cost; it does not remove the need for future reuse or complete delivery.

### 5.2 Component speedup cannot be promoted directly to agent speedup

Under an explicitly simplified serial decomposition, let f be the baseline episode fraction attributable to the component improved by factor s, with all other work unchanged:

\[
S_{\mathrm{episode}}=
\frac{1}{(1-f)+f/s}.
\]

For example, f=0.10 and s=2 imply only approximately 1.053× episode speedup. This is a hypothetical calculation, not a Suffice observation. Model inference, test execution, admission, and changed agent behavior can dominate or violate the simplified decomposition.

### 5.3 Hypotheses and their present status

| ID | Claim and controlled comparison | Required evidence | Present status |
|---|---|---|---|
| H1 | JSONL and binary yield identical ordered complete observations for the same admitted generation/query | Independent original-byte oracle; failures and arbitrary bytes | Supported within executed runtime tests/workload |
| H2 | Binary reduces complete tool-service cost on outputs sufficiently dominated by body bytes | Matched full-service arms; retain empty/tiny controls | Supported locally for four nonempty traces; empty control mixed |
| H3 | With identical rendered tool text, changing transport leaves model-visible tokens unchanged | Byte/token identity of rendered messages with fixed processor | Bridge strings matched in six paired observations; no Gemma tokenization/inference reported |
| H4 | Explicit generation validation reduces consumption of evidence mismatched to the chosen current revision during edit-heavy tasks | Controlled edit traces, actual mismatch labels, adapter ablation | Proposed; consistent historical reads alone are insufficient |
| H5 | The adapter preserves issue-resolution performance while reducing complete episode cost | Fixed model/harness/budget, independent task tests, paired task seeds, predeclared δ | Proposed; no task-success evidence |
| H6 | Named retention budgets predict or bound actual peak memory | Native/Python/consumer ownership and RSS traces | Unestablished; do not infer this from wire bytes |

### 5.4 Minimal credible next experiment

Use a fixed Gemma artifact and a reproducible developer-task harness on permitted public development tasks. Compare a straightforward baseline with generation-aware evidence delivery while keeping the retriever, prompts, task order, tools, test access, and budgets fixed. Treat text-identical JSONL/Binary as a separate transport ablation. Add independent repositories and a predetermined editing protocol, including unchanged, modified, renamed, removed, and newly created files.

Log the chosen generation at every observation and edit, result completeness, native/application errors, model-visible tokens, complete episode time, test outcomes, and actual memory. Evaluate current-generation mismatch separately from semantic relevance and solved issues. Report all failures and the overhead of capture/validation. Task/repository units, not individual tool calls, should drive the uncertainty analysis for task performance; repeated seeds must not be misrepresented as independent repositories.

A frozen task trace can isolate systems cost, while a live agent experiment measures behavior. They answer different questions: the live model may choose different actions after a small timing or context change. Do not merge their speedups.

## 6. Wording to avoid and replacements

| Avoid | Defensible replacement |
|---|---|
| “The first runtime to make agent context fresh” | “An explicit generation-bound artifact contract and measured complete-output implementation” |
| “Formally verified repository understanding” | “A conditional observation specification supported by executable checks” |
| “Binary cuts Gemma tokens in half” | “Binary reduces native-to-Python encoded traffic; token effects require a separate model-visible transformation” |
| “Every response is cryptographically proven correct” | “Admission verifies stored artifacts; the client validates structure; the experiment independently compares returned bytes” |
| “Pinned generations always describe the current checkout” | “Pinned generations preserve a named historical snapshot; current-workspace use requires coordinated revalidation” |
| “A code graph proves safe incremental reuse” | “A retrieval graph can guide relevance; reuse soundness needs complete dependency observations” |
| “Twofold agent acceleration” | “Local complete tool-service ratios; no Gemma episode speedup measured” |
| “Memory is bounded by the Worker budget” | “Named retained Worker components are bounded; transient and total process memory remain separate” |

## 7. Primary bibliography and local evidence

**R1a.** Google DeepMind. Gemma 4 model card. Official documentation, accessed 2026-11-12. https://ai.google.dev/gemma/docs/core/model_card_4

**R1b.** Google. Function calling with Gemma 4. Official documentation, accessed 2026-11-12. https://ai.google.dev/gemma/docs/capabilities/text/function-calling-gemma4

**R1c.** Google. Gemma 4 Prompt Formatting. Official documentation, accessed 2026-11-12. https://ai.google.dev/gemma/docs/core/prompt-formatting-gemma4

**R2.** John Yang, Carlos E. Jimenez, Alexander Wettig, Kilian Lieret, Shunyu Yao, Karthik Narasimhan, and Ofir Press. SWE-agent: Agent-Computer Interfaces Enable Automated Software Engineering. NeurIPS 2024. DOI: 10.52202/079017-1601. https://proceedings.neurips.cc/paper_files/paper/2024/hash/5a7c947568c1b1328ccc5230172e1e7c-Abstract-Conference.html

**R3.** Di Wu, Wasi Uddin Ahmad, Dejiao Zhang, Murali Krishna Ramanathan, and Xiaofei Ma. Repoformer: Selective Retrieval for Repository-Level Code Completion. ICML 2024, PMLR 235:53270–53290. https://proceedings.mlr.press/v235/wu24a.html

**R4.** Haojun Weng, Qianqian Yang, Hao Fu, Haobin Pan, and Xinwei Lv. When Retrieval Hurts Code Completion: A Diagnostic Study of Stale Repository Context. arXiv:2605.14478v1, 14 May 2026. Preprint. https://arxiv.org/abs/2605.14478v1

**R5.** Yuhang Wang, Yuling Shi, Mo Yang, Rongrui Zhang, Shilin He, Heng Lian, Yuting Chen, Siyu Ye, Kai Cai, and Xiaodong Gu. SWE-Pruner: Self-Adaptive Context Pruning for Coding Agents. arXiv:2601.16746v4, 7 May 2026. Preprint version consulted. https://arxiv.org/abs/2601.16746v4

**R6.** Andrey Mokhov, Neil Mitchell, and Simon Peyton Jones. Build Systems à la Carte. Proceedings of the ACM on Programming Languages 2, ICFP, Article 79, 2018. DOI: 10.1145/3236774. https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/build-systems-final.pdf

**R7.** Matthew A. Hammer, Khoo Yit Phang, Michael Hicks, and Jeffrey S. Foster. Adapton: Composable, Demand-Driven Incremental Computation. PLDI 2014, pp. 156–166. DOI: 10.1145/2594291.2594324. https://matthewhammer.org/adapton/

**R8.** Bazel. Skyframe. Official documentation, accessed 2026-11-12. https://bazel.build/reference/skyframe

**S1.** Fengji Zhang et al. RepoCoder: Repository-Level Code Completion Through Iterative Retrieval and Generation. EMNLP 2023, pp. 2471–2484. DOI: 10.18653/v1/2023.emnlp-main.151. https://aclanthology.org/2023.emnlp-main.151/

**S2.** Chunqiu Steven Xia, Yinlin Deng, Soren Dunn, and Lingming Zhang. Demystifying LLM-based Software Engineering Agents. FSE 2025. DOI: 10.1145/3715754. https://conf.researchr.org/details/fse-2025/fse-2025-research-papers/85/Demystifying-LLM-based-Software-Engineering-Agents

**S3.** Qizheng Zhang et al. Agentic Context Engineering: Evolving Contexts for Self-Improving Language Models. ICLR 2026; arXiv:2510.04618v3, 29 March 2026. https://arxiv.org/abs/2510.04618v3

**S4.** Thibaud Gloaguen, Niels Mündler-Sasahara, Mark Niklas Müller, Veselin Raychev, and Martin Vechev. Evaluating AGENTS.md: Are Repository-Level Context Files Helpful for Coding Agents? arXiv:2602.11988v3, 29 September 2026. https://arxiv.org/abs/2602.11988v3

**S5.** Tural Mehtiyev, Biruk Tadesse, Lawrence Arkoh, and Wesley Assunção. When Instruction Files Go Stale: Documentation Debt and Hidden Cost in Coding Agents. arXiv:2611.09802v1, 6 November 2026. Preprint. https://arxiv.org/abs/2611.09802v1

**S6.** Google/Kaggle. Google – The Gemma 4 Developer Agent Competition, official prediction-track overview, accessed 2026-11-12. https://www.kaggle.com/competitions/gemma-4-developer-agent/overview

**L1.** Preserved old paper packet: /workspace/scratch/8dfa229f9a87/deliverables_kaggle_suffice_2026/Suffice_Gemma4_Paper_Packet.zip, member Suffice_Gemma4_Paper_Packet/paper/Suffice_Kaggle_Writeup.md.

**L2.** Current report and result index: /workspace/scratch/8dfa229f9a87/suffice_v0_13/REPORT.md and /workspace/scratch/8dfa229f9a87/suffice_v0_13/results/transport_v0_13/SUMMARY.json.

**L3.** Architecture and protocol: /workspace/scratch/8dfa229f9a87/suffice_v0_13/RESEARCH_DOSSIER.md and /workspace/scratch/8dfa229f9a87/suffice_v0_13/docs/binary_protocol_v0_13.md.

**L4.** Client behavior inspected directly: /workspace/scratch/8dfa229f9a87/suffice_v0_13/scripts/catalog_client.py.

**L5.** New read bridge and final native validation: /workspace/scratch/8dfa229f9a87/kaggle_revision_v2/integration/repository_read_bridge.py and /workspace/scratch/8dfa229f9a87/kaggle_revision_v2/integration/validation_native/RESULT.json. Four processes, 114 checks, 12 complete observations, 36 expected rejections, and six cross-transport string equalities. This new correctness gate is separate from the frozen 0.13 performance experiment.

No source above is evidence that Suffice executed Gemma, solved an issue benchmark, achieved a token reduction, or was externally peer reviewed.
