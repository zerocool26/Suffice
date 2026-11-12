# Suffice 0.13 release evidence and validation review

## Decision and scope

**PASS for the inspected source, build, validation and measurement evidence, with the limits below.** No blocking identity or count inconsistency was found. This is an evidence-readiness review; the final 0.13 ZIP, release inventory and relocated package-audit receipt had not been produced at this review cutoff. Those need their own final packaging receipt.

The review read files, inspected the catalog executable's ELF dependency declarations and independently reconciled recorded hashes and counts. It did **not** execute a native binary, rerun a test suite, rerun the retained-record auditor or repeat a benchmark. The [machine-readable companion](release_validation_review_v0_13.json) records the reviewed file sizes/hashes, per-gate counts, build/cache ancestry and checks. The reviewer is a separate agent from the build, measurement and release author, but authored the Python client and its framing tests. That implementation received a separate [transport review](transport_review.md); this document is not external peer review.

## Exact executed gate counts

Counts below retain their original units. Correctness suites are nested within the build gates; rejection checks, row comparisons and injected failures are not additional independent test suites.

| Gate | Executed configuration | Recorded result |
|---|---|---|
| Direct native build and gates | Release | **9 binaries, 32 newly compiled application/test objects, 10 executable gates**, all passed; 74 project inputs unchanged |
| Direct native build and gates | ASan/UBSan | The same **9 binaries, 32 objects and 10 gates**, all passed; 74 project inputs unchanged |
| Python framing/connection suite | Final client | **26 test cases**, no failures/errors/skips; 192 validated terminal responses, 267 expected protocol rejections, 159 fragmentation positions, 155 truncation positions and 2 real-child EOF probes |
| Producer suite | Release packer | **22 test cases**, no failures/errors/skips; 33 expected rejections, 22 publications, 8 unchanged reuses, 82 rows / 4,356 bytes compared |
| Full catalog integration | Release/JSONL, Release/Binary, ASan/UBSan/JSONL, ASan/UBSan/Binary | **7 passed groups per arm**; each has 34 full-output calls, 1 hash-only call, 181 rows / 1,224 bytes compared, 14 rejected corruptions, 35 native rejections and 9 client-failure checks |
| Arbitrary-byte transport integration | Release and ASan/UBSan, each with JSONL/Binary × Capture/Recompute | **4 processes and 28 complete-output checks per build**; each build compares 92 rows / 26,215,296 bytes, checks 28 row-wire formulas, 4 streaming calls, 4 hash calls, 8 preflight rejections and 4 QUIT exchanges |
| Worker-only clear protocol | Release, both transports; two population strategies each | **6 complete-output checks per transport**, each comparing 8 rows / 153,411 bytes |
| Measured transport experiment | Release, 5 scenarios × 10 paired blocks × 2 transports | **100 fresh processes, 4,840 complete-output checks, 23,480 rows and 419,571,140 body bytes compared** |

Sources: [Release build manifest](../build/transport_release/build_manifest.json), [sanitized build manifest](../build/transport_sanitize/build_manifest.json), [final framing receipt](../results/client_framing/RESULT.json), [producer receipt](../results/producer/RESULT.json), the four `results/integration_*/RESULT.json` receipts, the two `results/transport_integration_*/RESULT.json` receipts, both `results/worker_*/RESULT.json` receipts and [measurement summary](../results/transport_v0_13/SUMMARY.json). Every canonical validation receipt was read; all **47 recorded source/binary identities across those ten receipts** match the inspected files. The earlier `client_framing_initial` and `client_framing_pre_range_check` receipts describe earlier client/test identities and are not the final gate.

The ten native gates in each build are five correctness programs—core, payload, catalog, digest and wire—and five CLI checks: core version, payload help/version and catalog help/version. Their substantive logs provide the following detail in **each** build:

| Native correctness program | Recorded coverage |
|---|---|
| Core | 17 named passed groups |
| Payload | 17 groups; 370 pipeline runs; 47 cache replays; 65 expected rejections; 15,261 rows / 740,069 bytes compared; 652 feature-bound checks; 1,615 certificate-row checks |
| Catalog | 26 groups; 104 successful runs; 39 expected rejections; 501 rows / 3,947,538 bytes compared; 12 injected allocation failures |
| Digest and path encoding | 29 SHA-256 vectors; 20 UTF-8 boundaries; hexadecimal round-trip and 4 rejection cases |
| Wire writer | 9 groups; 125 checks; 129 expected rejections; 36 poisoning cases |

These log hashes were checked against their manifests. Both builds enable assertions in test translation units. Neither the correctness calls nor the 4,840 within-process output checks should be advertised as independent timing trials: the timing observation unit is the matched process block, with ten blocks per scenario.

## Build and cache provenance

Both direct builds record GCC 14.2.0, a stable compiler-driver hash, stable project inputs and successful command/log identities. Independent inspection found no mismatch in **all 51 recorded commands/logs, 32 objects, 9 binaries and 10 gate logs per build**. The 51 commands comprise 32 compilation steps, 9 links and 10 gate invocations.

The core support archive was **reused**, not recompiled in these invocations. Both manifests explicitly say `cache_hit: true` and `this_invocation_build_ms: 0.0`. Each support identity covers the compiler configuration and 36 core source/public-header files. Recomputing the key from the recorded identity, hashing all current core inputs and hashing the actual archive produced these matches:

| Build | Support cache key | Support archive SHA-256 |
|---|---|---|
| Release | `efcabcf06f19df5942aaea4a8c2ba2a840315669e2dfd73acacd1f549fb2078e` | `68e48e33adfe9ef695da4664247b5ee00ee840455ee13d7f3c766233b6260744` |
| ASan/UBSan | `edeeb3f86581e92baa3ffe611f371ae704a8beac0b6c744b2acb6ef59d7aa7e9` | `3925b205213b4ef6b58a532d58a4020f467d394d83ab4c38340719c63752f38e` |

The preserved 0.12 ZIP contains the original build manifests. They record `cache_hit: false`, the identical core identities/archive digests and the same **16 original compilation/archive commands per configuration**. Each also has 16 relocated support-command logs plus an index under `validation/support_logs_release/` or `validation/support_logs_sanitize/`. Old absolute paths in the copied command lists are historical provenance. Thus the accurate release wording is: **new application/test compilation and linking against verified, unchanged 0.12 core support archives**.

The cache-hit implementation checks the archive against the cached manifest's digest; it does not separately compare the cached manifest's identity/key to the newly computed identity/key before returning. The independent comparisons above close that provenance question for these actual archives. An explicit identity/key comparison would strengthen future cache handling; this is not evidence that the measured archives were wrong. A source package can omit cached libraries and rebuild them locally.

## Preserved 0.12 release

The copied [baseline archive](../baseline/Suffice_0_12_0.zip) and the original release archive are both **6,957,408 bytes**, with SHA-256:

`da69f1f305452e1cffd89ddd50aea1009ba43ab1f7eee3e52624c2b60a170ec8`

This matches the original 0.12 release receipt. All six extracted baseline snapshots—README, report, project handoff, validation summary, release inventory and research dossier—also match their corresponding members in that archive. The new transport experiment uses the preserved predecessor corpus; it does not revise its measurements or redefine historical 0.12 as the new JSONL control.

## Measurement and portable audit boundary

The inspected [RUN](../results/transport_v0_13/RUN.json), [suite](../results/transport_v0_13/suite.json) and [SUMMARY](../results/transport_v0_13/SUMMARY.json) hashes link correctly. All **100 process-record hashes**, **78 frozen source identities** and **187 retained input identities** match; the current measured code still matches the frozen copies. The copied predecessor suite/run identities also match. Re-summing process/request records reproduces the reported output counts and cumulative traffic: **639,715,576 received bytes and 1,035,660 sent bytes** across both transports and all scenarios.

The portable auditor reads bundled relative files and validates the historical build identity, original-body descriptors, schedule, modeled cache/resource state, independent row-wire sizes, cumulative byte accounting and paired arithmetic. It uses Python's standard library, does not need the historical absolute source paths and does not execute the recorded binary. The complete input directory, frozen source, provenance copies and process-record inventory therefore belong in the final package. Their bytes, names and ordinary-file status must survive extraction; symlinks and noncanonical evidence paths are rejected.

This is retained-record verification. The historical run performed complete body equality, while saved records retain descriptors/digests rather than raw native response captures. The audit cannot replay a past pipe exchange, prove an original timestamp or restore a past host environment. Terminal-control byte counts are retained observations; the row-wire formula supplies an independent check of row traffic. The auditor itself and the original measurement recorder remain part of the trusted local evidence process.

A relocated package audit should be recorded separately, along with the exact ZIP payload inventory, member identities, archive hash and CRC check. This review did not observe those final artifacts and makes no claim that archive assembly has already passed. Legacy versioned packaging scripts remain historical tools; the documented 0.13 workflow must select the 0.13 evidence and release root.

## Remaining reproducibility and liveness limits

1. **Environment closure is incomplete.** The manifests explicitly mark default system header/library hashes as uncollected. Compiler-driver identity is not a hash of the entire toolchain, linker/archiver, SDK or runtime libraries. The inspected Release catalog declares dependencies on `libzstd.so.1`, `libstdc++.so.6`, `libgcc_s.so.1` and `libc.so.6`. The source is rebuildable with the stated dependencies; byte-identical fresh builds and arbitrary-host binary compatibility are not established.

2. **Sanitizer scope is specific.** The ten instrumented build gates passed with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Standalone integration receipts bind binary/script identities but do not persist their full inherited environment. The project build does not instrument the separately installed codec. These receipts support the exercised ASan/UBSan checks, not a leak-free result or instrumented coverage of all dependency internals. Execution receipts are Linux-only; Windows CRT handling remains supplied but unexecuted.

3. **EOF cleanup is narrower than a deadline.** The final framing suite includes one real-child probe for each transport in which stdout closes while stderr and the child remain alive. The client raises a protocol error and kills its owned child without attempting the former blocking stderr drain. Those two probes passed with an outer five-second test timeout. Normal stdin writes and stdout reads are still blocking; a child that keeps stdout open while stalling can wait indefinitely. Graceful `close()` first requests QUIT, so its later ten-second process-wait timeout does not bound a stalled QUIT response. Neither stderr-flood handling nor arbitrary-process cancellation is established by the EOF probes.

4. **Protocol bounds are not whole-process memory bounds.** Per-frame and aggregate row/body limits reject oversized output, but Python still materializes rows and may retain them with `collect=True`. Streaming callbacks receive a complete materialized row. The receipts establish the stated framing and retained-component contracts, not RSS ceilings, zero-copy delivery, concurrency safety or bounded total service time.

These limits do not invalidate the completed transport comparison. They determine the claims the report and package can support and the next separate engineering gates.
