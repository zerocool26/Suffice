# Independent review: fused result capture and proposed 0.12 experiment

Review date: 2026-11-11, America/Chicago. This is a source review of the new payload capture path and its tests, plus a review of the proposed experiment supplied by the implementation owner. It is not a new native execution, formal proof, Windows validation, or audit of a completed benchmark harness.

## Conclusion

**No blocking wrong-output, lifetime, range, output-limit, or candidate-publication defect was found in the inspected fused capture implementation under the documented nonreentrant Worker contract.** The new path preserves the existing complete-output and query-owner/range identity boundary while avoiding the separate counting and writing decode passes used by the retained Recompute control.

The implementation owner still needs the recorded native correctness/sanitizer gates and measurement run. Source inspection alone does not establish their outcomes. Source hashes at the end of this document scope the review; later changes require comparison against those identities.

## Scope inspected

- `include/suffice/payload.hpp`: `Worker`, `CaptureReport`, `ResultCache`, output limits, borrowed callback spans.
- `apps/payload/payload.cpp`: selection extraction, exact metadata preflight, candidate allocation, delivery/copy, legacy construction, replay identity, and decoder lifetime.
- `tests/test_payload.cpp`: new capture ranges, tails, complete-output comparisons, exact/undersized budgets, owner/range rejection, callback interruption, and corrupt-frame handling.
- Relevant integration call path in `apps/catalog/catalog.cpp` and current allocation-failure/strategy tests in `tests/test_catalog.cpp`.

The baseline comparison was the preserved 0.11 implementation. No reviewed runtime file was edited by this review.

## Correctness reasoning

### Selection and output preflight

`select` validates the requested range, normalizes the backend, resets the active mask, and fills it against the worker's immutable query/snapshot. `selection_size` enumerates that same active mask, masks the final partial word, and obtains decoded lengths from the same snapshot metadata. Row and byte additions use the existing checked arithmetic helpers.

`run_captured` checks both output limits before invoking the first sink callback. The selected bytes must fit the candidate budget before the code evaluates `budget - size.bytes`; short-circuit ordering makes the subtraction safe. Dividing remaining budget by entry size avoids an overflowing row-size multiplication, and the explicit `SIZE_MAX` checks precede vector-size conversion.

`deliver` checks decoded extent through the existing decoder, so an OnDemand snapshot cannot silently produce a body of a different length than preflight assumed. The ordinary `run` path now shares the extracted selection/delivery implementation and still performs finite-limit preflight before callbacks.

### Candidate ownership and exception behavior

The candidate is privately owned by a `unique_ptr`. Candidate construction, row-vector allocation, and body-vector allocation happen before delivery and catch `bad_alloc`/`length_error` as optional capture failure. Their failure does not prevent normal delivery through the null-candidate path.

The candidate retains the same shared query owner. It receives row IDs, offsets, lengths, and body copies only after each successful sink callback. A throwing callback or failed decode unwinds through the report and destroys the private candidate; no report containing a partial candidate returns. Earlier sink side effects may remain, consistently with the existing streaming contract.

Copy bounds are checked with subtraction before adding `decoded.size()` to the offset. The candidate is returned only after final row-count and byte-extent agreement. Zero-length bodies skip `memcpy`, and empty selections can produce a valid zero-storage result. Replay still requires the identical query owner and range.

The decoded span remains valid between the callback return and the subsequent copy because the Worker is explicitly nonreentrant and the next decode has not occurred. A callback that reenters or destroys that Worker violates the declared contract; no thread-safety guarantee is inferred from immutable snapshot data.

### Test coverage visible in the patch

The added payload tests cover every existing backend across nine ranges, including empty ranges, unaligned ranges, word boundaries, and the final row. They compare complete delivered and replayed rows with the independent fixture oracle; exercise exact and one-byte-too-small candidate budgets; reject wrong query owner/range before callbacks; verify finite output caps emit nothing; and verify recovery after callback and corrupt-frame failures.

The current catalog tests additionally exercise Capture and Recompute output equivalence and inject failure at the three optional candidate allocations after the query/worker have been warmed. They check successful delivered output and preservation of the prior cache. These are meaningful path-specific failures; their actual execution status belongs to the owner's native test receipts.

## Measurement and contract qualifications

### Diagnostic timing is not pure copy cost

Capture performs `steady_clock` calls around every copied row. That overhead is present only on the Capture path and remains charged to the actual implementation. `capture_ms` also includes selection-size scanning and candidate allocation; it is a diagnostic subset of the enclosing call, not an independent quantity to add again.

Report complete request/session cost as primary. Do not call the diagnostic field a direct measurement of bare `memcpy` cost or infer that subtracting it yields an uninstrumented counterfactual execution time. The per-row timing overhead can be removed or coarsened in a later separately measured revision if it is material.

### Retention budgets do not bound call peaks

The candidate exists while the normal decoder and consumer are active. Successful optional allocation can therefore increase peak memory before later decoder or consumer work. Catching the candidate's allocation failure provides a local fallback; it cannot guarantee that every later allocation would succeed as it would in a run without the candidate.

This is consistent with the current named retention budgets and separate transient/consumer memory boundary. It should remain explicit in the report. A future hard controlled-allocation cap requires reservations across the whole call, as described in `foundations.md`.

### Decoding counters need an exact scope

`RunStats::decoded_payload_bytes` describes the normal delivery run. In the Recompute control, the two extra passes execute inside `ResultCache::build`; the normal delivery counter does not automatically include that additional decoding. Removing those passes is a verified structural code change. Claiming a measured whole-session decoded-byte reduction additionally requires construction/admission counters covering all relevant paths.

## Proposed experiment review

The supplied plan uses the same immutable 103-file corpus, four scenarios, eight process blocks, a balanced four-condition Williams order for Capture/Recompute crossed with 16 MiB/256 KiB retained-worker budgets, and a two-budget bypass control. This is an appropriate descriptive factorial ablation when each condition has a fresh process and the corpus/output contracts remain fixed.

### Expected workload accounting

| Scenario | Requests per process | Conditions | Repeats | Processes | Requests |
|---|---:|---:|---:|---:|---:|
| Reuse | 48 | 4 | 8 | 32 | 1,536 |
| Pressure | 26 | 4 | 8 | 32 | 832 |
| Mixed query | 72 | 4 | 8 | 32 | 2,304 |
| Bypass worker churn | 48 | 2 | 8 | 16 | 768 |
| Total | — | — | — | **112** | **5,440** |

These totals are expectations from the design, not completed-result counts.

For four treatments A–D, one valid balanced order set is `ABDC`, `BCAD`, `CDBA`, `DACB`; each ordered distinct pair appears once across its adjacent transitions. Eight blocks can use each sequence twice. Record the actual sequence, block and process identities. Balance reduces first-order order effects; it does not establish exclusive CPU ownership or remove filesystem-cache and host interference.

### Required analysis distinctions

1. Report matched Capture/Recompute contrasts at each worker budget and tight/loose budget contrasts within each strategy. Inspect their interaction rather than claiming the effects are independent.
2. Keep the four scenarios separate. Do not combine them into one universal speedup. The eight matched process blocks, rather than 5,440 requests, are the repeated performance observations for each relevant contrast.
3. Bypass should vary only worker budget. Its purpose is to expose preparation/reconstruction costs when complete-result hits cannot hide worker behavior.
4. Keep input identities, output predicates, result budget, protocol, consumer, and oracle fixed within each contrast. For mixed queries, compute the oracle independently for the specific predicate; using one stored expected selection for every query would weaken the experiment.
5. Count worker reconstruction in the useful request/session cost. Report post-call retained worker bytes separately from active worker/component peaks and RSS. The 256 KiB setting is a retained-worker constraint, not a process-memory cap.
6. Preserve raw wall time alongside service time that subtracts only explicitly timed oracle comparisons. The oracle still affects cache/scheduling state; subtraction does not erase those effects.
7. Record all result-policy events, worker preparations/reuses/evictions, output checks, and failures. A run with changed hit behavior may still be informative, but the causes must be explained rather than attributed solely to the fused copy path.
8. Avoid running source builds or other agents' CPU-heavy validation concurrently with the timed experiment. Retain affinity, toolchain, CPU, source hashes, and execution order. External host contention remains a limitation.

With eight blocks, paired geometric ratios, paired medians, observed ranges, and direction counts are sensible descriptive outputs. Do not label the observed range a confidence interval or claim application-wide 10× performance. Native Windows and developer-agent task success remain separate validation gates.

## Scope identity

The JSON block below records reviewed file identities and the review-only status. It is generated from the local files after inspection.

Final-source refresh: all six identities below were compared again after the owner reported the completed Release and sanitizer build gates; every identity was unchanged. The final header was also inspected to confirm the new worker limits, population option, worker report flags, and worker statistics were appended after their respective existing aggregate prefixes. The refreshed review did not rerun or independently certify those build gates.

```json
{
  "status": "source review completed; no blocking defect found under documented contracts",
  "new_native_execution": false,
  "runtime_edits_by_reviewer": false,
  "experiment_review": "design only; expected totals are not measured results",
  "files": {
    "include/suffice/payload.hpp": {
      "bytes": 8831,
      "sha256": "f5110f982bdc02da0733bc4a73d90d4040a278589b00023bda1ffb9748d334d4"
    },
    "apps/payload/payload.cpp": {
      "bytes": 23477,
      "sha256": "1218278ae212362cc97e1046f064666003f905d6cc66000cf054936aeca735a5"
    },
    "tests/test_payload.cpp": {
      "bytes": 45915,
      "sha256": "be0d30c8df6e086fba4147d1c663ece779f9e7b549b0d7a45192cc25d9326390"
    },
    "include/suffice/catalog.hpp": {
      "bytes": 7055,
      "sha256": "ed581ba060a578bb8147b4a4d16fbbbc2fbdf322e77613b5669d246eeb8040d9"
    },
    "apps/catalog/catalog.cpp": {
      "bytes": 27422,
      "sha256": "21ea1f3c0e80e832b309a4c9e054801486ac92bb43b13d6d386300872bec5bd5"
    },
    "tests/test_catalog.cpp": {
      "bytes": 42148,
      "sha256": "12e9b7adda6797ab13e024434e5aa13b7a830f343f26f1cab85e6ca62a24c2c1"
    }
  }
}
```
