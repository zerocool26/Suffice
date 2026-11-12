# Suffice 0.13 continuation

## Current completed state

Read [REPORT.md](REPORT.md) and [README.md](README.md) first. Version 0.13 implements bounded binary native responses, strict complete-output client validation and wire-byte accounting. JSONL remains the default; Binary is explicit. Both return the same ordered row IDs, paths and complete bodies, using the same underlying query, Capture/Recompute and Worker policies.

The final Release and ASan/UBSan builds passed ten gates with unchanged source inputs. Final framing validation passed 26 tests; producer validation passed 22. Full integration passed for both transports under both builds. Arbitrary-byte transport integration passed both transport/population combinations under both builds, and Worker controls passed through each transport. Refer to final result directories, especially `results/client_framing/`; earlier parser-development receipts are historical checks, not the final source identity.

The scheduled experiment and retained-record audit both passed: **100 processes, 4,840 complete-output checks, 23,480 compared rows and 419,571,140 body bytes**. `results/transport_v0_13/SUMMARY.json` is the result index; its RUN/suite/process records and frozen measurement sources carry the detail. No native timings need to be reconstructed from this handoff.

The four nonempty scenarios have paired-geometric JSONL/Binary service ratios of 2.13808 (reuse), 1.79944 (pressure), 1.90233 (mixed queries) and 2.02757 (computed). All ten blocks favor Binary in each of those scenarios. Median complete-session response-byte reductions are approximately 49.45–49.59%.

**Preserve the adverse control:** empty output uses approximately 2.30% more response bytes under Binary. Its service geometric ratio is 1.07010, but its paired median is 0.99119 and only four of ten blocks favor Binary. It does not establish a consistent empty-output improvement. Keep all observations and raw-wall comparisons.

## Historical continuity

`baseline/Suffice_0_12_0.zip` preserves the unchanged 0.12 release and its earlier lineage. The `baseline/v0_12_*` files preserve its report, handoff, validation/inventory and research overview. The original competition-paper study remains historical; do not relabel its measurements or the earlier 0.12 retention experiment as new 0.13 observations.

Inherited research covers validity, representation, economic retention, dependency witnesses, ownership and reservations. The new work implements response transport and client checks, not those proposed larger mechanisms.

## Contracts that future edits must preserve

- SFC1 framing has explicit endianness, validated type/version/reserved fields and exact nested extents. Frame limits and cumulative request limits are different obligations.
- Native write/flush failure permanently poisons output. No ordinary terminal may follow a partial frame. Client malformed/truncated output and callback errors close the child; only a fully consumed, validated native terminal error preserves synchronization.
- Full output, hash-only diagnostics, future handles and future deltas have different consumer contracts. Include final resolution/application costs before comparing different representations.
- Result identity is immutable generation ownership, exact source bytes and range, with the required Query owner. Generation-local row IDs cannot be reused as stable artifact identities across edits.
- Worker limits bound retained component state after an operation settles, including exceptions. They do not bound active transient memory or process RSS.
- The measurement compares two transports inside one 0.13 source/binary/client configuration. Its primary complete-service metric includes startup, Full admission, transfer/materialization, statistics and close, minus explicitly timed oracle/audit intervals. Query-only timings are secondary.
- Ten matched process blocks per scenario are the timing observations. Thousands of request/output checks are not independent timing replications. The single corpus is split into twelve logical partitions.
- Encoded pipe bytes, logical body bytes, physical I/O, allocation, retained components and RSS must remain separately labeled.
- The portable auditor checks saved evidence and arithmetic. It does not re-execute historical native output or recreate historical timing.

## Next implementation priorities

### 1. Define and test request liveness

Framing bounds accepted bytes; it does not establish a deadline. The client fixes the stdout-EOF/still-live-stderr stall, but an otherwise live child that stops producing output can still block synchronous reads. Request deadlines and cancellation require a platform-aware contract for the owned child, completed callbacks, blocked writes and cleanup. Distinguish requested cancellation from completed reclamation. Preserve exact-prefix semantics and prevent later requests from consuming abandoned output.

Measure slow consumers, a child that stays alive without finishing a frame, blocked output and interrupted callbacks. Bound queues and live ownership rather than reporting that a cancellation request alone freed resources. These are new mechanisms and require new regressions and evidence.

### 2. Execute native Windows validation

`scripts/run_windows_v0_13.ps1` is provided for the Windows 11 target and has not run here. Configure the existing MSVC builder path, x64 compiler/archiver and matching Zstandard headers/library/runtime, then run the full framing/producer/integration workflow in new directories. Record actual native compiler, CRT and filesystem outcomes before making compatibility claims.

The native code sets stdin/stdout binary mode before server I/O. Linux tests do not exercise the Windows CRT branch. Keep a separate Windows performance protocol; the current timing harness requires Linux affinity APIs.

### 3. Measure complete memory behavior and consumer backpressure

Obtain process RSS/high-water evidence and allocation measurements separately from protocol wire bytes and Worker retention. Include collection versus streaming, large outputs, slow callbacks, cache pressure and simultaneously live owners. Binary output still copies through stream buffers, pipes and Python objects; no zero-copy or proportional RSS reduction follows from the wire result.

Only after defining the controlled allocation graph should a reservation scheduler claim an invariant such as live controlled bytes plus outstanding reservations at most B. Account for shared snapshots, results, external leases and incomplete asynchronous work.

### 4. Broaden workload evidence before changing the default

Retain the empty-output control and add independent repositories, body-size/selectivity distributions and real consumer traces. Keep cold admission and complete consumption in the reported endpoint. The current result supports an explicit binary option; it is not a production-wide speed guarantee.

Cross-generation content/feature sharing and complete dependency witnesses remain a separate research track. Handle absent inputs, directory membership, toolchain/extractor identity and shifted row ordering before claiming exact reuse across changed generations.

## Reproduction and preservation

Use new output directories. The direct builder records native inputs, toolchain identity and gate status, and refuses an existing destination. The completed 0.13 result bundle can provide its own corpus/stores for fresh measurements. The portable audit resolves included paths and requires no original external workspace location.

The Python client/framing tests and portable audit use the standard library. Producer/capture and fixture-producing integrations require Python `zstandard`; native execution requires its separately installed codec dependency. The supplied native Windows workflow is a future execution path, not a receipt.

Do not edit the retained `measurement_source/`, original inputs, process records, protocol/method snapshots or completed RUN/SUMMARY files to make later code appear measured. Create a new experiment for changed code, contracts or platform conditions.
