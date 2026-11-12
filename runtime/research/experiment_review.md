# Independent review of the Suffice 0.13 transport experiment

## Decision and scope

**PASS after two retained-record checks and one timing-label correction.** No blocking execution, oracle, timing-boundary or paired-analysis defect remains in the reviewed harness and method document. This review was completed before the timed matrix ran; it does not report experimental outcomes.

The reviewer was a separate AI-assisted agent from the harness author and also implemented the Python client. This is an independent harness consistency review, not independent external peer review of the whole system. Review consisted of reading the harness, method document, client telemetry and native row writer, plus small standard-library calculations of schedule counts and ratio arithmetic. No native benchmark, build, sanitizer run or additional runtime integration test was executed for this review.

## Reviewed identities

| File | Bytes | SHA-256 |
|---|---:|---|
| `scripts/benchmark_transport_v0_13.py` | 53,485 | `edd889bc32902304a79c2e719174841cc505567281272e703b4cd55831f6a3c9` |
| `docs/transport_experiment_v0_13.md` | 11,096 | `69f65d65c2f16d6b72091fcf81fcef4688c715a4074105aa29e2e056e3eed754` |
| `scripts/catalog_client.py` | 20,673 | `f0282195bf939aded456aa54258bcf0c237ccd9255660d665875feaaf6680495` |
| `apps/catalog/wire.hpp` | 6,052 | `064d9a4f9e9bb993d8f1afdbf592e033af7f883046c6b631e52abe714b53002d` |

## Output identity and oracle

The oracle reads the preserved original source bodies, checks their recorded lengths and SHA-256 identities, assigns rows from UTF-8 path order within each project, and applies body-length predicates directly. It does not select using the runtime's derived feature columns. Every measured RUN compares row ID, order, path and every original body byte. Empty output is a legitimate checked answer.

The supplied source corpus has 103 files distributed into twelve deterministic logical partitions; it is not twelve independent repositories. The experiment copies the existing 187 input files and captured stores without recapture or re-encoding. Three nonempty thresholds and the empty-output threshold remain explicit. Adding the empty threshold contributes zero result capacity, so it does not alter the mixed-query result budget.

Both transport variants use one binary/client, Capture population, the same full-output API, the same FNV work and the same runtime retention settings. Native final policy counters and retained resource state must match within every paired block. The independent result-LRU model continues to account for admissions, hits, complete victim sets and byte/entry occupancy under the fixed-generation, full-range experiment contract.

## Client telemetry, framing and close

The native JSONL row field order matches the independently reconstructed compact JSON envelope. Its expected row-wire size is that envelope with an empty body plus twice the original body length and one LF. The binary formula is 16 header bytes plus 32 row-prefix bytes plus UTF-8 path length and body length. Neither calculation uses a native reported output byte count in place of the original-body oracle.

The client increments cumulative received bytes for every actual read, including partial header/payload reads, and cumulative sent bytes for every accepted write. Successful request telemetry partitions the consumed response into complete row and terminal-control bytes. The harness retains startup, each OPEN and its STATS, each RUN and its STATS, and the terminal QUIT returned by close_result. Their contiguous sequences and summed request/response counters must equal the cumulative counters after close.

Terminal control lengths remain observed client counters. They are not an independent packet capture, and the experiment does not claim to reconstruct their exact historical textual floating-point representations. Framed transport removes hex encoding but still materializes Python byte strings. Wire-byte reduction is not a measured reduction in physical disk I/O, allocations, RSS or elapsed time.

## Timing boundary

Raw wall time starts before process creation and stops after successful close. It includes readiness, all twelve Full admissions, runtime computation/population, complete transport, Python validation/materialization, STATS requests, remaining bookkeeping and shutdown. The client and both transport conditions use the same full-output consumer contract.

Explicitly timed output equality, descriptor/hash work, LRU/resource audits and row-wire audits are subtracted from the primary service metric. Their recorded startup/admission/query components sum back to the total audit time. Consistency checks performed after close occur after the raw clock has stopped and are not subtracted again. The saved timing-contract label now names the wire audits explicitly, matching the code and method document.

Raw wall remains available as a sensitivity check. Subtracting observer time does not reverse its cache, allocator or scheduling effects. Query-request sums are labeled as a secondary measure that excludes startup, admission, statistics and shutdown. Native phase timers overlap broader clocks and cannot be added as independent causal components.

## Schedule and arithmetic

A separate small calculation confirmed the default matrix contains 100 fresh processes and 4,840 complete-output requests. Every scenario has five JSONL/Binary orders and five Binary/JSONL orders. Across ten repetitions, each scenario occupies every scenario position once under each order; each scenario-position/transport-position combination appears exactly once. This supports the stated within-pair and scenario-position balance; it does not establish exclusive hardware or remove all temporal interference.

Each scenario's samples are indexed by repetition before dividing JSONL by Binary. Geometric means are computed from those matched ratios, not from ratios of independently aggregated medians. The service reduction corresponding to ratio g is 1 − 1/g. Per-block wire reductions preserve negative values, so the empty-output case can legitimately report a traffic increase. No unlike-scenario request pooling or confidence interval is introduced.

The observation unit for one timing contrast is ten matched fresh-process blocks. The 4,840 correctness checks are not 4,840 independent latency trials. The corpus, shared pinned Linux CPU and lack of cache flushing remain explicit limits.

## Identity and portability

Preparation requires a passed nonsanitized native build with stable compiled inputs and successful gates. It checks the actual executable against its build-manifest identity, then freezes project inputs, the Python client/driver and both method documents. Current source/input identities are checked before each paired scenario group and after the complete run.

The portable auditor uses bundled inputs, source copies, manifests and process records. It does not open historical absolute source paths or execute the native program. Relative-path validation rejects parent traversal, absolute/drive-style paths, backslashes, control characters and symlink traversal; the frozen-source and input inventories reject extra or changed files. Raw record identities, the schedule and paired summaries are linked through saved hashes. These checks establish consistency of retained evidence, not cryptographic proof that recorded timings occurred.

## Corrections made before the measurement freeze

1. The raw record already contained native_argv, but the auditor did not compare it with the selected transport and controlled arguments. It now requires the exact invocation assembled from the recorded binary, serve, the transport choice and the fixed native options.
2. The method required matching request traffic, but only final native state was checked across paired variants. The summary now requires identical cumulative request bytes within each matched pair and records matched_request_wire_bytes.
3. The saved timing-contract description omitted wire auditing from its exclusion label. The wording now includes it; the measurement implementation already timed and subtracted that work correctly.

No runtime algorithm or experimental observation was changed by these review corrections. Executed correctness gates and later measured outcomes belong in their own receipts and release report.
