# Suffice 0.13: self-contained evidence verification

This folder accompanies the revised paper about exact repository observations. It verifies the **completed 0.13 transport experiment**. It does not run a model, native Suffice executable, new benchmark, external download or dependency installer.

## Start here

Open `Suffice_Offline_Verification.ipynb` and run its five code cells in order with Python 3.12 or later. The complete evidence ZIP and verifier are embedded. No attached dataset, GPU, API key, internet access, `zstandard` Python package or historical workspace path is required.

The notebook contains saved outputs from a local standard-library cell-execution driver. That execution is documented in `VERIFICATION_RECEIPT.json`; it is **not** represented as a Kaggle or Jupyter UI run. No public upload or competition submission was performed by this workflow.

The result table and SVG display every scenario, including the mixed empty-output result. The table and figure are derived from all fifty matched process pairs, then checked against the frozen auditor's summary. Each process pair is one timing observation; the 4,840 requests are correctness checks within processes.

## What is bundled

`Suffice_0_13_Offline_Evidence.zip` contains exactly **372 regular files**:

| Group | Files |
|---|---:|
| Original corpus copies and captured stores | 187 |
| Frozen measured C++/Python source and method documents | 78 |
| Historical process records | 100 |
| Suite, RUN, SUMMARY, measured build manifest and two predecessor-provenance copies | 6 |
| Original MIT license | 1 |

The ZIP is **3,837,891 bytes** and expands to **27,339,265 bytes**. Its SHA-256 is:

`c6ba35462a6e1e997808aa68efda16e75ec5ec8a6e0812e59cc2a3df0ef57eac`

The frozen auditor is the unmodified measured file `results/transport_v0_13/measurement_source/scripts/benchmark_transport_v0_13.py`, with SHA-256:

`edd889bc32902304a79c2e719174841cc505567281272e703b4cd55831f6a3c9`

The export preserves every measured file byte and relative name. The archive manifest lists each member's length and digest. Native executables, build objects/caches, third-party research PDFs, earlier release archives and competition-provided task data are absent. The included model-independent corpus is the project's own preserved predecessor code.

## What passing establishes

The retained-record auditor verifies the original inputs and source identities; corpus-derived expected output descriptors; complete balanced schedule; modeled result-cache and resource state; independent row-wire size formulas; complete request/response byte reconciliation including QUIT; and paired timing/wire arithmetic. Its recomputed summary agrees with the saved summary.

The checked experiment comprises **100 processes, 4,840 complete-output checks, 23,480 returned rows and 419,571,140 compared body bytes**. The latter is repeated delivery across requests, not unique corpus size. The unique corpus has 103 files and 1,460,207 plaintext bytes.

The local notebook validation executed all five code cells, preserved source identity, and observed zero Python socket/process-launch events. While the frozen auditor ran, 943 file open/directory-scan events were checked, with zero accesses outside the newly extracted evidence tree. The temporary tree is cleaned up after the audit. The guard is a validation aid, not a security sandbox for untrusted Python.

The export was additionally checked against all 372 per-member hashes, its ZIP CRCs and the exact embedded notebook payload. `EXPORT_CLOSURE.json` records final artifact identities and these closure checks. `OFFLINE_AUDIT_RESULT.json` records compact paired observations and audit results.

## What passing does not establish

The original native run compared every returned body byte. This notebook checks saved descriptors/digests against original bodies; it does not replay the historical pipe responses or reproduce their timestamps. It does not decompress payload frames or rerun the source-admission, parser, allocation-failure or sanitizer suites. Those have separate evidence in the full runtime release.

A source/build hash is not a hermetic build proof. One shared pinned Linux configuration and one corpus do not establish universal speedup, native Windows behavior, task-success improvement or total-memory ceilings. Pipe bytes are not model tokens or RSS. No Gemma inference is represented here. Any newly added completed-read bridge has its own later integration receipts and is not part of the original 100-process experiment.

## Run from a command line

From the revised packet root:

```bash
python3 verification/run_offline_audit.py \
  --report verification/local_audit.json
```

The report path must be new. The default ZIP path is resolved relative to the runner, so the command can also be invoked from elsewhere. The runner uses only Python's standard library and temporary local files.

To repeat the notebook cell-execution check, use a **new output directory** for its receipt, report and figure:

```bash
python3 verification/validate_notebook.py \
  --notebook verification/Suffice_Offline_Verification.ipynb \
  --receipt verification/local_cell_run/receipt.json \
  --audit-report verification/local_cell_run/audit.json
```

This updates the notebook's saved execution outputs while keeping its code/Markdown source identity fixed. Keep the provided original notebook and receipts if the historical executed artifact must remain unchanged; run the driver on a copy in that case. `local_cell_run` must be fresh because the driver also writes its derived SVG there.

For a clean export from the exact completed runtime tree, use a new output directory:

```bash
python3 tools/build_verification.py \
  --source /path/to/Suffice_0_13_0 \
  --output /path/to/new_verification
```

The builder requires the recorded suite/RUN/SUMMARY/auditor identities, preserves the entire retained result directory and creates an initially unexecuted notebook. The validator above then creates fresh execution receipts. ZIP compression can depend on the installed zlib version; the generated wrapper pins the exact ZIP it just built. Do not relabel regenerated wrapper/ZIP bytes as the preserved published artifact.

`Suffice_0_13_Paired_Observations.svg` is the standard-library notebook figure. The PNG is a visually checked preview rendered separately; it is not required by the notebook or its standard-library execution.

## Interpretation

Four nonempty scenarios favor Binary in all ten matched blocks, with paired geometric complete-service ratios from 1.799 to 2.138. Empty output favors Binary in only four blocks, has a paired median below one, and uses approximately 2.30% more response bytes. The plot retains this adverse control. These are observations under the same declared complete-output contract, not claims that a coding model became more capable.
