# Suffice 0.12 continuation

## Current work and evidence

Read `README.md` and `REPORT.md` first. The executable changes are worker retention independent of prepared queries, exception-safe retained-worker caps, worker-only clearing, and complete-result capture during normal decoding. Default Use still does not populate on a miss. Explicit Populate now selects Capture by default; Recompute remains the controlled legacy-construction comparison.

The 0.11 competition paper and its experiment remain preserved. New 0.12 observations are not substitutions for those historical numbers. `baseline/Suffice_0_11_0_Code.zip` and `baseline/Suffice_0_11_Paper.md` preserve the compact baseline. Selected old validation records are under `baseline/validation_v0_11/`.

`research/` contains the new cache-economics and foundations reports, their primary-source catalogs, and independent AI-assisted reviews. `docs/architecture_v0_12.md` connects those results to the broader project. Each research proposal is distinguished from the current implementation.

## Preserve these distinctions

- The enforced worker limits describe retained component capacity after an operation settles, including throws. Active transient allocation and RSS are separate.
- Capture removes two construction decode traversals structurally. Existing pipeline counters do not include the Recompute builder's extra traversals.
- The measured comparison is within one 0.12 binary, with a controlled construction strategy and worker budget. It is not a rerun of the old 0.11 timing on an identical host state.
- The complete service metric includes startup, Full admission, output transfer/materialization, instrumentation and close, minus explicitly timed oracle/audit intervals. Raw wall time remains available.
- Each scenario has eight repeated process blocks, not thousands of independent latency trials. The corpus remains one real predecessor tree split into twelve logical projects.
- An offline audit checks retained records and arithmetic. It does not rerun original timings or establish historical observations independently of their source.
- Native Windows, whole-process memory, disk-allocation savings, model performance and agent task success remain unmeasured here.

## Next implementation decision

Choose the next mechanism from measured complete-session cost. The three proposed experiments are binary transport under unchanged full-output semantics, dependency-witness validity across edits, and shared reservations/cancellation under ten-project pressure. Their protocols and counterexamples are in `research/foundations.md`.

Before adding an admission predictor, compare an inexpensive bounded history baseline and keep construction and postconstruction retention decisions separate. Account for complete victim sets and shared ownership. Never convert unknown dependency validity, exhausted proof budgets, or failed validation into a cache hit.

## Reproduction

Use a fresh build and result directory. The builder refuses existing output directories and records source/compiler identities and correctness gates. The completed 0.12 experiment bundle can provide its included immutable corpus for a fresh measurement; no previous absolute predecessor path is required. The portable audit requires only Python's standard library and opens bundled evidence rather than recorded external paths.

Native Windows validation has a dedicated runner at `scripts/run_windows_v0_12.ps1`; it is supplied for the target machine and was not executed on this Linux host. Do not rebrand the Linux affinity-controlled timing matrix as a Windows experiment.
