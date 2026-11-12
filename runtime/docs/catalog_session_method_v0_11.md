# Complete-session experiment — Suffice 0.11

The experiment in `scripts/benchmark_catalog_v0_11.py` measures the new persistent
catalog at its application boundary: process startup, full admission of twelve
generations, a request trace, complete file-byte delivery through the Python
client, explicit result-cache construction, statistics requests and shutdown.
It compares two policies inside the same persistent implementation. It does not
claim a comparison with Cargo, an operating system, an optimized database or a
production development workload.

## Corpus and preparation

The corpus consists of the exact `.cpp`, `.hpp`, `.py`, and `.suf` files beneath
the preserved 0.10 `src`, `include`, `apps`, `scripts`, and `examples` directories.
Cache directories are excluded. Paths are sorted by their UTF-8 relative-path
bytes, and file at index `i` is assigned to logical project `i mod 12`. Original
relative paths and all file bytes are preserved. This creates twelve partitions
of real code; it does not turn them into twelve independently sampled user
projects.

Preparation records every original path, partition, byte length and SHA-256,
copies the bytes into the experiment's input directory, and uses the producer to
capture one fixed immutable generation per partition. Producer capture reports,
generation manifests, all component files and complete original input copies are
retained. The preparation record describes corpus copying, feature extraction,
compression and packing time separately. These generations are shared unchanged
by both session policies. Their construction is not repeated inside each native
session.

The predicate is deliberately simple and identical in every partition:

```text
input byte_length 0 0
output byte_length > 1024
```

An independent oracle reads the preserved original files, compares them to the
copied input bytes, and applies Python's `len(bytes) > 1024`. It does not use the
producer's features, native predicate implementation, selection mask, or output
hash to determine expected selection. Row IDs come from the separately sorted
original path inventory. The native generation loader independently validates
its feature/body/metadata association before publication.

## Session traces and policies

| Trace | Requests | Cache capacity |
|---|---:|---|
| Reuse | Four cycles through projects 0–11: 48 requests. | Sum of the twelve complete selected-result capacities. |
| Pressure | Four hot 0 / hot 1 pairs, a single scan through 2–11, four more hot 0 / hot 1 pairs: 26 requests. | Exactly enough retained result capacity for the two hot results together. |

Result capacity is the selected plaintext length plus the inherited three-uint64
row entry per selected row. The authoritative observed `cache_storage_bytes`
must match this preparation model on the measured platform; otherwise the audit
rejects the experiment. The cache entry limit is 64. Query-plan capacity is 12, so
this pressure trace concerns result retention, not an additional plan-LRU
pressure experiment.

The two policies are:

- **Persistent bypass:** one process admits every project once and reuses
  prepared plans/workers, while result-cache lookup and retention are bypassed.
- **Persistent populate:** the same admission and exact output contract. A miss
  first delivers the complete direct result, then performs the actual inherited
  two-pass cache build. A hit emits the complete cached plaintext through the
  same native hex writer and Python decoder.

Populate is an explicit retention policy, not a learned selector. The first
request includes its direct output plus two additional cache-construction
passes. No construction phase is moved outside the measured session. This first
use differs from the 0.10 build-before-replay model; its old payback threshold
must not be carried into this experiment.

Both policies receive identical per-request output limits, sized from each
partition's complete input row/byte dimensions. Both receive the same snapshot,
metadata, project, plan and result limits. No consumer is allowed to replace a
complete byte transfer with a cached handle or a fingerprint.

## Pairing and process state

Each trace uses six fresh-process pairs. Pair order alternates bypass/populate
and populate/bypass; trace order also alternates by pair. Each process starts
with an empty registry, no plan and no result. The harness checks this readiness
state, then opens all twelve frozen generations before executing the trace.

On a host supporting affinity, the harness pins its own process to one permitted
logical CPU; native children inherit that affinity. The original affinity is
recorded and restored. This is not exclusive CPU ownership and does not remove
shared-host interference. Both native and Python output work compete for that
CPU, which is part of this stated complete-client configuration.

Filesystem caches are not flushed. Input-identity checking reads the frozen
inputs before process pairs, so filesystem pages may already be warm. A fresh
process is therefore a new application state, not a cold-storage assertion.
No timed process runs concurrently with another process from this experiment.

## Timing boundary

The primary observation is

`service_ms = raw_session_wall_ms - oracle_validation_ms`.

`raw_session_wall_ms` runs from before native process creation through clean
shutdown. It includes process readiness, all full admissions, requests, native
planning/execution/cache construction, output serialization, pipe transfer,
Python JSON and hex decoding, materialized byte objects, and a STATS request
after every query. Control bookkeeping and statistics validation are included.

`oracle_validation_ms` is the explicitly timed, post-request comparison of every
returned row ID, path, length and byte with the original data, plus diagnostic
output-row digest construction. That validation is outside each request's wall
timer. The raw session observation is also retained so the subtraction remains
visible and auditable. This is a measured instrumented service session; it does
not estimate away STATS protocol or client overhead.

The record additionally retains:

- Startup-to-ready and close times.
- Every OPEN's outer request time, native admission time, generation identity,
  manifest hash, verified-body bytes and component bytes.
- Every query's outer request time and client-reported time.
- Native total request, plan, execution and optional cache-build times.
- Every observed cache-hit/admission decision and the complete statistics state
  after every request.
- Per-request and total counts of fully compared output rows and bytes.

These nested phase timings must not all be added together: native phase values
are already inside the complete request time. Preparation, pre/post-pair
identity checks, oracle loading, and final evidence-file serialization are
outside the native session. One-time capture/packing costs remain available in
`suite.json`; a brand-new project's capture-plus-session cost must include them.

## Correctness and resource audit

The Python client creates fresh collected row objects from each request's
actual JSON/hex stream. After every request, the harness checks the complete
ordered `(row, path, bytes)` sequence against the independent original-file
oracle. Equality is byte-for-byte, not merely hash equality. The raw outputs are
not redundantly saved for every process; their exact original bytes, executed
comparison code, row/path/length/digest descriptors and comparison counts are
preserved.

The summary independently reconstructs expected descriptors from the original
files and verifies every retained record. A separate small byte-LRU model
replays the request trace and checks hits, admissions, victims, retained bytes,
entry counts, and decoded-byte counters at every step. It also records whether
any observed admission needed more than one victim. A trace need not contain a
multiple-victim event; the native correctness suite separately tests that case.

After every request, observed logical snapshot, metadata, result and count
limits must hold. There must be twelve admitted snapshots and no retired input
owners in this no-update experiment. `plan_worker_bytes` is retained as a
component observation. These are not whole-process RSS or backend-specific peak
memory measurements. Output materialization in Python and transient admission
buffers remain outside the retained input/result limits.

## Provenance and summaries

Preparation freezes the producer, client, experiment script, payload writer,
native implementation inputs, build helpers, both native executable identities,
and the passed stable-input build manifest. Native/producer/client/benchmark
identities are rechecked before and after each pair. Every prepared input's
inventory, length and SHA-256 are also checked. Measured source files are copied
into `measurement_source` and checked against their original hashes.

Each process record has its own hash in `RUN.json`. The experiment refuses to
overwrite or resume an existing run. An incomplete or failed run cannot be
summarized as passed. The summary validates input/source snapshots, record
inventory, pairing, trace order, identities, row descriptors, exact LRU behavior,
and the service-time subtraction before computing statistics.

For a metric `T`, each pair yields

`r_i = T_bypass,i / T_populate,i`.

The headline descriptive ratio is the geometric mean of these six paired
ratios. Values greater than 1 favor Populate. The minimum and maximum are
observed process-pair ranges, not confidence intervals. Median policy times are
reported separately and must not be used to reconstruct the paired geometric
ratio. Negative or near-parity findings are retained.

## Practical limits

The experiment evaluates one predicate, one fixed corpus partition, two session
traces and one Linux host with a Python hex-output client. Its real code corpus
does not establish production query frequency, representative entropy, project
mutation rates, storage contention or Windows performance. The selected-byte
fraction can be much higher than selected-file fraction, making output work
dominant. The pressure trace is a defined scan-pollution diagnostic, not a
distribution of users' daily work.

This comparison isolates the decision to retain complete results within an
already persistent admitted catalog. It does not measure the advantage of
persistence against reopening and readmitting every project on every request.
It also does not evaluate a cost-aware or scan-resistant replacement policy.
Such changes require new measured sources and a new evidence directory.

## Completed observations

The official evidence is in
[`results/catalog_sessions_v0_11/SUMMARY.json`](../results/catalog_sessions_v0_11/SUMMARY.json),
with the preserved process records, input generations and measured sources in
the same directory. All source/input identity checks and independent output/LRU
audits passed. The run contains 24 fresh native processes, 888 complete-output
checks, 6,504 compared rows and 111,050,700 compared plaintext bytes.

The corpus has 103 files and 1,460,207 plaintext bytes. The query selects 86 files
but 1,455,100 bytes: approximately 83.5% of files and 99.65% of bytes. Thus most
plaintext still crosses the complete output boundary on every request.

| Trace | Paired service ratio GM, bypass/populate | Observed pair range | Pairs favoring Populate |
|---|---:|---:|---:|
| Reuse | 1.019242 | 0.947262–1.182204 | 2 of 6 |
| Pressure | 0.905022 | 0.828404–1.076655 | 1 of 6 |

The reuse result does not establish a consistent service-time advantage: four
of six pairs favor bypass even though the geometric mean is slightly above 1.
The pressure trace favors bypass in five of six pairs. The paired geometric
pressure ratio corresponds to approximately 1.105 times the service time for
Populate; this is a descriptive result from this fixed trace and host, not a
confidence statement or a general caching theorem.

Median policy times, using a different aggregation from paired ratios, were
77.594 ms bypass versus 81.079 ms Populate for reuse, and 51.954 ms versus 61.253 ms for
pressure. They should not be divided to reconstruct the paired geometric means.
Native query-phase and complete-client query-request ratios are retained in the
summary alongside full-service and raw-wall observations.

Every Populate reuse process recorded 36 hits, 12 admissions and zero evictions.
Every Populate pressure process recorded 12 hits, 14 admissions and 12 evictions.
The scan removes both hot results, and the first returning request for each hot
project rebuilds its cache. No admission in these particular traces required
multiple victims; that behavior is covered by the native lifecycle suite.

All processes retained 444,574 logical Snapshot input bytes and 19,552 generation
metadata bytes. After all projects had been queried, the 12 retained workers
reported 1,693,703 component bytes under both policies. Reuse result storage was
1,457,164 bytes; pressure result storage was 283,059 bytes. Every step matched the
independent byte-LRU model and stayed within its configured limit.

The worker observation matters for resource design: a compact compressed input
does not imply equally small prepared execution state. The current logical
input/result limits and the separately exposed worker number should remain
distinct. The experiment did not measure process RSS or a laptop memory ceiling.

One-time corpus copying, capture and packing took 389.470 ms in this preparation.
That figure is outside each fresh-process service window and is preserved in
the suite record; startup of a brand-new uncaptured project would also pay its
own preparation. Native full admission is inside every measured service session.

These results support keeping result admission explicit. They motivate a future
comparison with a scan-resistant or saved-work-aware policy and an examination
of prepared-worker storage. They do not justify silently making Populate the
universal default.

## Auditing a relocated download

The frozen benchmark's original `summarize` command verifies its recorded
absolute predecessor-source path. Preserve that script as measured. For a
download extracted somewhere else, use the separate standard-library auditor:

```sh
python scripts/audit_catalog_archive_v0_11.py --project-root . --report my_archive_audit.json
```

The report path must be fresh. Add `--require-binaries` to require every Release
executable described by the recorded build; otherwise any available archived
Release executables are checked and missing ones are reported explicitly.
The auditor requires the full evidence distribution. The compact current-source
package deliberately omits the raw experiment input tree.

This tool reads the included corpus copies and frozen measurement sources,
checks every generation manifest/component identity, independently derives all
four feature channels and row mappings, checks the exact recorded output
descriptors, and replays the byte-LRU model. It recomputes the logical snapshot
total, process schedule, timing sums/subtraction, paired ratios, policy medians
and complete comparison totals. The byte-identical measured Release build
manifest is preserved beside the experiment in `MEASURED_BUILD_MANIFEST.json`.

Historical absolute source/tool paths remain provenance strings. The relocated
auditor does not reread the external predecessor tree, compiler installation or
SDK, and does not execute the historical native outputs again. Its report
separately states the archived-byte identity checks, recorded-descriptor audit,
available-binary checks and original experiment's full-byte comparisons.

`audit_checks/RESULT.json` records two successful audits in a disposable
relocated view without copied native binaries. During those audits a Python
file-open guard rejected any input-file open outside the view; none occurred.
Six intentional corruptions were rejected: changed or missing original-copy
bytes, changed frozen source, changed paired statistics, changed output
descriptors with refreshed outer record hashes, and changed LRU counters with
refreshed outer record hashes. These are audit checks, not additional native
performance observations. The official measurement files were left unchanged.
