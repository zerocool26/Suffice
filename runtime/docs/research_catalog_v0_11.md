# Suffice 0.11 design research: persistent catalog, validity, and bounded reuse

Research date: 2026-11-11 (America/Chicago). This is a design brief for the next release, not a claim that every recommendation below has been implemented or benchmarked. It follows the preserved 0.10 `PROJECT_HANDOFF.md` and `docs/research_agenda_v0_10.md`. The companion `sources_catalog_v0_11.json` records primary sources, retrieved references, and the exact role of each source.

## Decision

Build a persistent **catalog of immutable admitted generations**, with exact reuse inside each generation and explicit memory accounting across projects. Start with serialized publication and a bounded conventional eviction baseline. Make filesystem event acceleration optional. This delivers an application boundary that can be implemented and measured on a solo Windows laptop without claiming to replace a compiler, Cargo, or the operating system.

The important distinction is among three contracts:

1. **Captured bytes:** the catalog owns an immutable set of byte strings and derives features from those exact strings.
2. **Filesystem capture:** a declared protocol determines which path observations enter a generation, including errors and exclusions.
3. **Application commit:** a writer manifest, immutable source tree, or snapshot establishes whether multiple files belong to one coherent application version.

The first can be guaranteed by the existing ownership model. The second requires new ingestion code. The third cannot be inferred from successful sequential reads. Keep these claims separate in APIs and reports.

The 0.10 experiment already showed that common payload work often dominates the difference between exact gates. The next experiment should therefore measure persistent-session admission, queries, updates, cache retention, and output together. It should include the time spent building the catalog and bytes duplicated on disk, because the user's actual problem is many concurrent projects consuming storage and memory.

## 1. Publication: one immutable generation at a time

Recommended generation identity:

`Generation = {project_id, generation_id, source_manifest, feature_schema_id, extractor_version, payload_format_version, admitted_snapshot}`.

Recommended query identity:

`QueryKey = {generation_owner, exact_program_or_semantic_id, range, output_contract}`.

These fields are design recommendations. The exact existing `Query` owner is the safest initial reuse key: textually identical programs can still denote different computations when feature interpretation changes.

### Publication protocol

1. Capture the current generation and a publication token under the catalog lock.
2. Build a candidate privately. Derive features and payload association from the same captured bytes; enforce input, row, output and construction limits.
3. Validate the candidate completely under the declared policy.
4. Reacquire the publication lock and require the expected generation/token to match. A slow older build must not overwrite a newer published build.
5. Publish the complete candidate as one operation. Failed construction, validation, or a stale token leaves the active generation unchanged.
6. Existing readers retain the old generation; new readers acquire the new generation. Reclamation follows actual ownership, not removal of the project's current-pointer entry.

A mutex-protected shared owner is a valid first implementation. A C++ atomic shared owner is another publication mechanism, but its lock-free property is implementation-defined, and object initialization must itself be synchronized. Atomic pointer replacement does not make mutable payload contents safe. [S01]

Generation IDs should be monotonically allocated within a catalog lifetime, with explicit overflow rejection. Durable identities need a restart epoch or persisted unique ID. Never infer generation equality merely from a recycled address or a per-process counter restarted at zero.

Rows are **generation-local positions**. Inserting or removing a file can shift every later row. Keep artifact path/content identity separate from row IDs, and never replay a mask or result against another generation because its dimensions happen to match.

### Disk persistence is a separate commit

A persistent generation can be an immutable directory containing data plus a manifest. Write and validate a candidate directory before making a reference to it current. Never publish a path to partially written files. On restart, accept only complete committed generations and identify incomplete owned staging directories for later cleanup.

Visibility after a process operation and durability after power loss are different guarantees. Microsoft's `ReplaceFileW` documentation states that its write-through flag is unsupported and describes failure states; a generic rename wrapper is insufficient evidence of a durable multi-file transaction. [S08]

SQLite transactions are an alternative for a small catalog's commit metadata. Separate database connections expose committed states, and WAL readers can retain a database snapshot while a writer commits. A transaction in SQLite does not atomically commit unrelated external blob files: finish and validate immutable blobs first, commit references second, and collect only unreferenced owned blobs later. SQLite's atomic-commit behavior also depends on filesystem and flush assumptions. [S14, S15]

## 2. Windows ingestion: events are hints, bytes establish identity

### Start with an explicit capture command

An explicit, bounded capture is enough for the first usable catalog. It also gives a reproducible oracle for later watcher implementations. Enumerate only declared project roots and include/exclude rules. Record unreadable files, skipped reparse points, size-limit failures, and enumeration failures. Default publication should fail if the requested capture cannot be completed; an optional partial capture needs an explicit coverage contract visible to consumers.

For each admitted file, open one handle, read bytes through it, and derive the payload and cheap features from those bytes. Compare relevant handle metadata before and after the read and retry or reject detected changes. This catches common races; it does not prove that a concurrent writer never produced mixed bytes, or that all files came from one global instant. A file may also change between distinct file reads. Label this mode a captured observation set unless the writer supplies a stronger commit boundary.

Windows share modes can deny conflicting opens, at the cost of interfering with editors and builds or failing on already-open writers. Reparse-point behavior must be chosen deliberately; `FILE_FLAG_OPEN_REPARSE_POINT` opens the link itself rather than following it. A checked final component alone does not establish a fully race-free traversal of arbitrary changing ancestor directories. [S06]

Windows file identity can combine the volume serial number and 128-bit file ID when available. File IDs are useful for handle identity and rename reconciliation, but are not content digests and are not permanent identities across deletion/reuse. [S05, S16]

Store paths using a defined relative encoding and ordering. Reject ambiguous duplicate keys. Do not globally lowercase paths: Windows supports per-directory case sensitivity, relevant to the user's WSL/native workflow. Preserve original spelling and choose explicit behavior for case-sensitive directories and case-only renames. [S07]

### Add a watcher only after reconciliation is correct

`ReadDirectoryChangesW` can lose detailed events on buffer overflow. A successful call with zero returned bytes requires directory/subtree enumeration; asynchronous completion obtains the transferred byte count through the completion mechanism, not the synchronous output parameter. Model overflow, restart, and uncertainty as **reconciliation required**, not “no changes.” [S02]

A watcher should invalidate a capture candidate or schedule another scan. It should not patch published immutable bytes in place. Coalesce repeated events and bound the pending queue. During a burst, retaining one dirty-root marker is safer and smaller than an unbounded list of filenames.

The NTFS USN journal can support recovery while the service was stopped. Its cursor includes both journal identity and USN. Old records can be deleted, and journal replacement/discontinuity invalidates assumptions. The documented journal operations have privilege requirements, so keep this as an optional Windows adapter rather than the baseline for an ordinary user process. [S03, S04]

If a true filesystem/application snapshot is required, evaluate a writer commit manifest or immutable source checkout first. VSS provides a Windows coordination framework for consistent snapshots, including application writers; it is a substantially larger integration than a file watcher and should not be silently assumed. [S09]

## 3. A shared budget must count retained ownership

Define accounting before selecting a policy. A useful ledger is:

`accounted_live = active_snapshot_bytes + retired_pinned_snapshot_bytes + retained_result_bytes + query/worker_charges + reservations`.

Count shared allocations once. A cache may keep an otherwise retired generation alive, so the marginal retained cost of that cache can exceed its own output vector. The correct marginal cost depends on the ownership closure and on which other owners remain alive.

Removing a cache from an LRU map does not reclaim memory if another caller holds it. Either expose scoped leases and keep their charges until destruction, or state that the limit bounds only manager-retained objects. A hard live-memory promise also needs control over public owners, worker creation, outstanding reads, and construction reservations. Ordinary allocator overhead, external libraries, and the operating system remain outside a simple logical-byte ledger.

For a first implementation:

- Bound result bytes, entry count, query-key bytes, and admission-history size.
- Reserve construction space before expensive work; release reservations on every exit.
- Refuse an oversized entry without evicting useful entries first.
- Determine the complete eligible victim set before changing the cache; allocation or publication failure should not leave a partially mutated index or double-counted ledger.
- Treat pinned entries as ineligible victims. If insufficient reclaimable space exists, bypass caching or return a declared admission failure.
- Serialize cache-index/accounting mutation; run decoding and callbacks outside the lock.
- Ensure callbacks cannot accidentally reenter mutable worker state. Separate worker leases per concurrent call.

### Admission and eviction are different choices

LRU gives a clear baseline but a one-pass scan can replace a repeatedly useful working set. TinyLFU uses recent approximate frequency to decide whether a candidate deserves admission relative to an eviction candidate; its bounded history and aging are relevant to query scans. Its published performance is evidence for its evaluated workloads, not for this catalog. [S10]

Variable-size complete results require more care. The size-aware TinyLFU study distinguishes policies that compare against one victim from a policy that compares candidate frequency with the aggregated frequency of the full displaced victim set. This matters when one large result would evict many smaller hot results. [S11]

GreedyDual-Size provides a classical cost/size/locality comparison, while SIEVE and S3-FIFO offer useful alternatives when policy metadata and hit-path synchronization become expensive. They belong in a measured policy comparison, not as automatic defaults justified by published benchmark ratios. [S12, S17, S18]

The catalog's objective should be useful avoided work under a budget. A prospective per-entry estimate is:

`V_i = expected_remaining_reuses_i * (uncached_time_i - replay_time_i) - construction_time_i - maintenance_time_i`.

This is a decision model, not a proved optimum. Past frequency is only an estimate of future reuse. Bytes copied to the consumer remain part of both execution paths. A density heuristic `V_i / marginal_live_bytes_i` can help choose a candidate set, but interacting shared owners and variable sizes mean independent rankings can make poor global choices.

A second-hit policy can be a cheap scan-resistant experiment. It does not establish payback: the 0.10 cache model had conditional median payback at three or four uses, with variation, and did not include changing generations or eviction. Report construction work wasted on entries invalidated before reuse.

## 4. Invalidation: start exact, then reduce its scope

The safest initial rule is whole-generation invalidation. It is coarse but explicit. A future dependency-aware layer can reduce recomputation only after it records every observation that affects the result.

Build Systems à la Carte separates scheduling from the decision to rebuild and explains dependency traces, early cutoff, and rule self-tracking. Its practical discussion includes untracked inputs, impure tasks, and inconsistent reuse of nondeterministic intermediates. These are directly relevant to a catalog eventually supporting build artifacts. [S13]

Recommended dependency categories are content, existence/absence, directory membership, rule/extractor identity, tool identity, configuration, and relevant environment. For example, creating a formerly missing optional configuration file can change a result even though every previously read file retains its digest. A glob can change when a new matching filename appears. Changing an extractor changes feature meaning without changing payload bytes.

For deterministic extractors, recompute affected values, compare the resulting canonical value, and stop propagation when the observable value is unchanged. For tasks involving time, external services, or mutable shared state, make the dependency explicit or declare the task volatile. Do not silently cache it as a pure function.

The first catalog release need not implement arbitrary build dependency discovery. Its source manifest and schema identity are the narrow, verifiable foundation for that future layer.

## 5. Implementation priorities and acceptance evidence

| Implement now | Purpose | Evidence required |
| --- | --- | --- |
| Immutable project generation registry | Stable owner and exact source/query identity | Reader retains old complete result after successful publication; new reader gets new result |
| Conditional publication | Prevent stale builds overwriting current data | Deliberately reverse two candidate completion orders |
| Bounded explicit capture | Useful artifact input without watcher complexity | Edits, additions, removals, rename, permission/read failure, limits, Unicode/case cases |
| Byte-budgeted result reuse | Share retention across projects | Zero budget, oversized entry, pinned owner, eviction, failed construction, accounting invariant |
| Exact output under updates | Preserve original result contract | Complete byte comparison for cache hit/miss/eviction and all generation transitions |
| Persistent commit/restart path | Resume without rebuilding unrelated files | Failed/staged writes remain unpublished; valid committed generation reopens |
| Session measurements | Test the real intended benefit | Cold admission, first answer, repeated queries, update cost, total bytes, live/retired retention |

Defer native watchers/USN, VSS, online learned policies, cross-generation query masks, arbitrary build execution, and transparent filesystem integration until the earlier contracts hold. Native Windows execution remains an explicit validation gate; reviewed source alone cannot supply it.

### Workload design

Use ten project namespaces with a small repeated query set interrupted by one-time scans, alternating edits, and a few large result sets. Include a giant cold result competing with many small hot results, zero-result queries, project switching, and slow readers retaining an old version. Keep a no-cache baseline and a byte-budgeted LRU baseline. Every policy must return identical complete output.

Record full-session latency, admission/build time, query miss/hit time, bytes read/written/decoded/delivered, retained and retired logical bytes, reservations, eviction/admission reasons, and peak process memory separately. A policy that improves hit ratio but increases total latency or retained disk bytes is a negative result worth preserving. Do not extrapolate synthetic trace behavior into a universal tenfold improvement.

### Counterexamples that should remain permanent regression cases

1. A file inserted before an old row changes row numbering; an old mask must not select the wrong payload.
2. A failed or stale capture must not replace the published generation.
3. Eviction while a reader holds a cache must not free borrowed output or falsely report reclaimed bytes.
4. An oversized result must not flush the useful cache before being rejected.
5. An unreadable file must not silently disappear from a supposedly complete generation.
6. A new matching file or formerly missing config must invalidate dependency results that observed the directory or absence.
7. A watcher overflow must cause reconciliation.
8. Reusing a path or file ID after deletion must not inherit old content identity.

## Source references

- [S01 — C++ working draft, atomic shared ownership](https://eel.is/c++draft/util.smartptr.atomic.shared)
- [S02 — Microsoft, ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
- [S03 — Microsoft, Using the Change Journal Identifier](https://learn.microsoft.com/en-us/windows/win32/fileio/using-the-change-journal-identifier)
- [S04 — Microsoft, READ_USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-read_usn_journal_data_v0)
- [S05 — Microsoft, FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)
- [S06 — Microsoft, CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [S07 — Microsoft, Case Sensitivity](https://learn.microsoft.com/en-us/windows/wsl/case-sensitivity)
- [S08 — Microsoft, ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
- [S09 — Microsoft, Volume Shadow Copy Service](https://learn.microsoft.com/en-us/windows-server/storage/file-server/volume-shadow-copy-service)
- [S10 — Einziger, Friedman, Manes, TinyLFU](https://arxiv.org/abs/1512.00727)
- [S11 — Einziger, Eytan, Friedman, Manes, Lightweight Robust Size Aware Cache Management](https://arxiv.org/abs/2105.08770)
- [S12 — Cao, Irani, Cost-Aware WWW Proxy Caching Algorithms](https://www.usenix.org/conference/usits-97/cost-aware-www-proxy-caching-algorithms)
- [S13 — Mokhov, Mitchell, Peyton Jones, Build Systems à la Carte: Theory and Practice](https://simon.peytonjones.org/assets/pdfs/build-systems-jfp.pdf)
- [S14 — SQLite, Isolation](https://www.sqlite.org/isolation.html)
- [S15 — SQLite, Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [S16 — Microsoft, BY_HANDLE_FILE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-by_handle_file_information)
- [S17 — Zhang et al., SIEVE](https://www.usenix.org/conference/nsdi24/presentation/zhang-yazhuo)
- [S18 — Yang et al., FIFO Queues Are All You Need for Cache Eviction](https://pdl.cmu.edu/PDL-FTP/Storage/FIFOqueues-SOSP23_abs.shtml)
