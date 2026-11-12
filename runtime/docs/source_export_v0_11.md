# Suffice 0.11 — curated source distribution

Suffice_0_11_0_Code.zip is the full code export accompanying the paper and
offline verification notebook. Its internal root is Suffice_0_11_0_Source,
matching the reproduction layout in the verification README.

## Code and evidence have different roles

This ZIP contains the current C++/Python implementation, headers, tests,
examples, build scripts, CMake configuration, catalog documentation, primary
source catalogs and selected final validation records needed to inspect the
implementation and build a new experiment.
The original code and MIT license are preserved byte-for-byte.

The separate verification/Suffice_Offline_Audit.zip contains the historical
measurement evidence: exact original corpus copies, frozen measured source,
captured generations, process records and the portable auditor. The notebook
verifies those records without compiling or running Suffice.

Use both exports for a new native experiment. The code export provides all
build/test inputs; the verification export provides the exact 103-file
predecessor corpus, which can be reconstructed using the supplied helper.
Historical timings are not reproduced merely by checking their saved records.

## What is included

- All source and input files under src, include, apps, scripts, tests and examples,
  excluding interpreter caches and compiled artifacts.
- CMakeLists.txt and the original MIT LICENSE.
- Current artifact capture, catalog API/protocol, session method, Windows,
  implementation-review and design-research documentation.
- The current primary-source catalog and review addendum as bibliographic JSON,
  without downloaded third-party papers.
- Unchanged final Release/sanitizer build manifests, producer result, both final
  integration results/execution receipts, and independent session-audit result
  and review. These are small validation metadata, not binaries or large fixtures.
- A new export README and SOURCE_INVENTORY.json.

Older source scripts remain available for inspection. Historical result datasets,
baseline archives and old builds are intentionally omitted; this export does
not claim to supply every input for replaying every older-version script.
The supported reproduction path here is the current 0.11 build and a new
catalog-session experiment.

## Integrity and provenance

The original full release is Suffice_0_11_0.zip, SHA-256:

    50d65fc0a921dd98144a747c7831d6b15e85197b5d4469540d083d4f096aac12

SOURCE_INVENTORY.json records each packaged file's size and SHA-256 and identifies
the newly written export README separately from original files. It confirms:

- All 48 frozen measured source files have identical corresponding current code
  in the export, with no exception.
- All project-file inputs in the successful recorded native Release build
  manifest are present and have identical bytes.
- Original selected source/document bytes match the completed release archive.

The inventory excludes its own hash to avoid a circular self-reference.
SOURCE_EXPORT_RECEIPT.json, supplied beside the ZIP, records the ZIP hash,
inventory identity, CRC/file-hash verification and the scope of the export.
No native build, test suite or timing experiment was rerun while creating it.

The selected validation records make the paper's correctness section inspectable.
Their recorded absolute paths and executable hashes describe the historical
execution; this code export does not include those historical binaries or all
regression input fixtures. Current test sources can generate new validation
evidence when a reviewer runs them.

The export includes no native executable, compiled object/library, interpreter
cache, old release archive, large result dataset or third-party paper PDF.
Dependencies are not vendored. The MIT license describes the supplied project
code; separately obtained dependencies retain their own licenses.

## Dependencies and building

Use Python 3.12 or later, a C++20 compiler, a static-library archiver available
with the toolchain, and native Zstandard headers/library. Source capture also
requires Python's zstandard package. The measured configuration used GCC 14.2,
Python 3.12.14, Python zstandard 0.25.0 and codec 1.5.7 on Linux x86-64.

The build script installs or downloads nothing. From the extracted source root:

    python3 scripts/build_payload.py --output build/paper_reproduction

The output directory must be new. The default command builds the native programs,
runs the recorded correctness/smoke gates, and creates a new build manifest.
It does not claim that the new executable will have the historical binary hash.

The inherited payload benchmark uses SQLite through a dynamic adapter; the
catalog itself does not require a SQLite database. Native Zstandard is required
for the payload/catalog build.

CMake files are included, but the completed release was built using the direct
Python builder; this export does not add a new CMake validation result.

For native Windows, use docs/windows_catalog_v0_11.md and
scripts/run_windows_catalog_v0_11.ps1 with an x64 MSVC developer shell and explicit
Zstandard include/library paths. Native Windows execution was not established
by the historical Linux results.

## New benchmark procedure

Follow the accompanying verification/README.md to reconstruct corpus_v0_10 as a
sibling of Suffice_0_11_0_Source. Keep the new native build and that source corpus
available and unchanged during the experiment.

From the extracted source root, after the build above:

    python3 scripts/benchmark_catalog_v0_11.py prepare --source ../corpus_v0_10 --binary build/paper_reproduction/suffice_catalog --packer build/paper_reproduction/suffice --output results/paper_reproduction
    python3 scripts/benchmark_catalog_v0_11.py run --output results/paper_reproduction --pairs 6

Preparation captures the corpus into fresh immutable generations. The run command
executes 24 native processes, compares complete outputs and automatically
summarizes the new run. It produces new observations and identities.

Do not overwrite historical records or rewrite their absolute provenance paths.
Use the portable auditor for old relocated evidence and fresh directories for
new builds/measurements. No GPU, model inference service or competition-provided
data is needed for this systems experiment.

## Availability

This is a prepared source package. It has not been publicly uploaded or submitted
by the packaging workflow. For public review, publish this code ZIP alongside the
verification notebook/evidence under freely accessible links. Private saved-file
links alone do not establish public availability.
