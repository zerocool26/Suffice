# Suffice paper revision

**Suffice Exact Repository Observations for Developer Agents**

This revision replaces the earlier 0.11-centered manuscript with a focused systems/resource paper based on the completed 0.13 transport experiment. It also supplies a separately implemented and native-tested repository read bridge. No Gemma inference or task-success result is claimed.

## Start here

| File | Purpose |
|---|---|
| `paper/Suffice_Paper.pdf` | Finished academic paper with two tables, a paired-observation figure, native equations, and seven references |
| `paper/Suffice_Kaggle_Writeup.md` | Canonical manuscript for the Kaggle Writeup editor |
| `paper/Suffice_Paper.docx` | Editable Word version of the same manuscript |
| `Submission_Checklist.md` | Completed artifact checks and remaining entrant/publication steps |
| `verification/Suffice_Offline_Verification.ipynb` | Self-contained notebook; all five code cells executed successfully |
| `verification/README.md` | Audit scope, evidence identities, execution and rerun instructions |
| `integration/README.md` | Narrow repository read tool, actual native test results, and integration limits |
| `source/Suffice_0_13_0.zip` | Unmodified latest runtime release, source, frozen experiment, and historical validation receipts |
| `support/PAPER_VERIFICATION.json` | Word counts, artifact identities, numerical consistency and visual review |
| `support/Evidence_Map.md` | Map from the manuscript's claims to retained evidence |
| `support/Kaggle_Fields.json` | Exact title, subtitle, abstract, and file mapping for entry preparation |
| `MANIFEST.json` | SHA-256 and byte count for every packet member except the manifest itself |

Only `paper/Suffice_Kaggle_Writeup.md` is the competition manuscript. The code, data, audit output and working research records are companions; do not append them to the Writeup. The PDF and DOCX express the same manuscript. Figure files are under `paper/figures/`; upload the PNG in the Kaggle editor and replace its local Markdown path with the editor's image link.

## What the evidence establishes

The frozen experiment comprises 100 fresh processes and 4,840 exact-output checks. In four nonempty traces, paired geometric JSONL/binary service ratios are 1.799–2.138; mean complete-session response-byte reductions are 49.45–49.59%. The empty-output control is mixed in timing and sends about 2.30% more bytes in binary mode. All observations, including unfavorable ones, remain in the data.

The separate read bridge passes 114 checks in four native processes, including six byte-identical cross-transport observations and failures after a real output prefix. Its overhead is not included in the frozen timing study. Its arguments are generation-local row ranges; it does not implement path search, semantic retrieval, edits, a Gemma parser/template round trip, or a complete coding agent.

The notebook audits retained records and original-byte descriptors. It does not reconstruct historical execution, decompress every saved packed payload, execute native binaries, or run a model. Its five cells were run in order with a standard-library driver; Kaggle/Jupyter UI execution is not claimed. The receipt reports zero observed network or external-process events.

## Reproduction and attribution

Use the notebook for the portable retained-evidence audit. See its README for command-line equivalents. Fresh native reproduction requires extracting the source release and building with a suitable C++20 compiler and codec dependencies; the historical direct builds reused verified unchanged core archives and are not fully hermetic.

The prior paper packet and the 0.13 release remain unchanged. `source/SOURCE_EXPORT_RECEIPT.json` identifies both. The new bridge is outside the frozen 0.13 measurement source. Existing MIT notices are preserved. Entrant rights and conditional competition winner-license obligations remain separate checks.

AI assistance supported implementation, research, analysis, reviews and writing. The scoped reviews included here are not external human peer review.

## Submission state

This packet has not been published or submitted to Kaggle. The entrant must complete the account/rights checks, provide public project links if used, preview the Writeup, and actually press Submit. Saving a draft or receiving these files does not create an entry. Consult `Submission_Checklist.md` and the live competition pages before submission.
