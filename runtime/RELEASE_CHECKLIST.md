# Suffice 0.13 release checklist

This checklist covers the bounded complete-output transport milestone. It does not certify competition eligibility or the unexecuted future work below. Executed checks are linked to retained evidence; package assertions are verified by `scripts/package_release_v0_13.py` before a successful release receipt is issued.

## Completed implementation and evidence

- [x] Continue from the verified complete 0.12 archive; preserve its original bytes and lineage.
- [x] Define a versioned, explicitly encoded binary response format with frame, path and aggregate output limits.
- [x] Preserve complete ordered row/path/body output and retain JSONL as the compatibility default.
- [x] Handle short reads, truncation, invalid lengths, wrong sequences, invalid JSON fields and count/range mismatches.
- [x] Make partial native writes and failed flushes permanently fatal to the writer.
- [x] Preserve only fully validated application errors as recoverable responses.
- [x] Close the owned child on malformed streams and callback failures; fix stdout-EOF cleanup blocking on stderr.
- [x] Add Windows CRT binary mode before serve I/O; keep Windows execution status unverified.
- [x] Record and reconcile complete request/response wire bytes, including terminal frames and QUIT.
- [x] Pass Release and ASan/UBSan direct builds, each with nine binaries, ten executable gates and stable project inputs.
- [x] Pass 26 final Python client/framing tests and the native writer fault fixtures.
- [x] Pass both transports under Release/sanitizer full integration and arbitrary-byte integration.
- [x] Preserve producer transaction and Worker-clear behavior under the final client/runtime.
- [x] Fix the review-found explicit-count hole in hash-mode success/error validation; retain regression cases.
- [x] Freeze the method, compiled inputs, client, driver and immutable corpus before measurement.
- [x] Execute balanced transport order and scenario rotation over ten paired blocks per scenario.
- [x] Compare every full output against original bytes; retain all scheduled observations and the empty-output control.
- [x] Report adjusted service, raw wall, narrower query time and exact application stream bytes separately.
- [x] Independently review raw-record arithmetic, file identities, pairing, wire accounting and influential observations.
- [x] Explain corpus, platform, statistical, sanitizer, build-provenance and liveness limits.
- [x] Supply source, protocol/method documents, measured evidence, charts, research program and continuation handoff.

## Package completion gate

- [x] Check final gate receipt identities against active sources and binaries.
- [x] Preserve original core-cache provenance and identify reuse explicitly; omit disposable build objects/archives.
- [x] Audit the relocated retained evidence without rerunning native historical measurements.
- [x] Verify archive CRCs, unique paths, every payload hash and inventory closure.
- [x] Issue a release receipt naming the exact archive SHA-256 and file count.

The last five items describe the successful packaged release. The external `RELEASE_RECEIPT.json` is authoritative; an interrupted or failed packaging attempt cannot claim them from this checklist alone.

## Open gates, not represented as completed

- [ ] Native Windows build and correctness execution.
- [ ] Absolute request deadlines, cancellation and bounded independent stderr draining.
- [ ] Combined native/Python RSS or Windows working-set/private-byte measurements.
- [ ] Whole-project disk-footprint and multi-project mutation measurements on the intended laptop.
- [ ] Independent repository and workload sampling beyond twelve partitions of one source tree.
- [ ] Compression, shared memory, content references, deltas or concurrent scheduling.
- [ ] Authentic model-agent task measurements and a fresh competition-specific compliance review.

See `VALIDATION_SUMMARY.json`, `REPORT.md`, `RESEARCH_DOSSIER.md`, `PROJECT_HANDOFF.md` and the packaged evidence audit for the scope and evidence behind each completed item.
