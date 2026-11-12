# Native Windows catalog — Suffice 0.11

The release was executed on Linux x86-64 with GCC 14.2. Native Windows has **not
been executed** in this environment. This guide and the PowerShell runner define
the target validation path; successful Linux tests are not Windows certification.

## Dependencies

Use an x64 Visual Studio Developer PowerShell with a C++20-capable MSVC toolchain,
Python 3.12 or later, and an x64 Zstandard header/library pair. Install the Python
`zstandard` module in that same Python environment. The recorded Linux producer
used Python zstandard 0.25.0 and native codec 1.5.7. The supplied script does not
download packages, modify system configuration, or install a daemon.

The native executable needs the Zstandard DLL on PATH when linked dynamically.
A static Zstandard library does not require the corresponding runtime DLL. Match
the library architecture and compiler/runtime conventions to your environment.
SQLite is only used by the inherited payload benchmark; the catalog and its
validation do not require SQLite.

From the extracted project directory:

```powershell
.\scripts\run_windows_catalog_v0_11.ps1 `
  -ZstdInclude C:\deps\zstd\include `
  -ZstdLibrary C:\deps\zstd\lib\zstd.lib `
  -ZstdDllDirectory C:\deps\zstd\bin `
  -ReleaseDirectory build\catalog_windows
```

Paths above are examples: supply the locations of your installed dependency.
The release directory must be new. The runner builds the native core, payload and
catalog applications, runs their correctness checks, then runs the real capture
and persistent-process integration checks. Preserve its manifest and validation
directory when reporting a platform issue. The Linux ASan/UBSan gate is separate;
the runner does not claim to provide those sanitizer checks under MSVC.

## Capture one source project

```powershell
python scripts\artifact_catalog.py `
  --root C:\work\MyApp `
  --catalog C:\work\SufficeCatalogs\MyApp `
  --packer build\catalog_windows\suffice.exe `
  --expected-current none
```

The initial capture fails if CURRENT already exists. For later guarded updates,
pass the actual previous generation ID to `--expected-current`. A capture without
that argument still uses the publisher lock but does not assert a caller's prior
generation. Every capture rehashes admitted source content; timestamps alone do
not establish unchanged bytes. Identical paths/content/features reuse the current
generation and do not recompress the files.

Defaults exclude build/dependency folders and bound each file to 1 MiB, the
decoded capture to 128 MiB, retained catalog files to 1 GiB, and generations to
16. Oversized admitted files cause a clear failure. Select an appropriate source
subtree or explicitly adjust producer limits; the native CLI still uses its
documented per-row admission limit. This release is intended for bounded source,
configuration and diagnostic artifacts, not arbitrary large game/media assets.

Use a quiescent source for a coherent project generation. Checked individual file
reads and repeated inventory scans do not create a cross-file transaction.

## Reuse an admitted project

Use the Python API from a script whose import path includes `scripts`:

```python
from catalog_client import CatalogProcess

query = "input size 0 0\noutput size > 1024\n"
with CatalogProcess(r"build\catalog_windows\suffice_catalog.exe") as runtime:
    opened = runtime.open("myapp", r"C:\work\SufficeCatalogs\MyApp",
                          expected_revision=0)
    first, rows = runtime.run("myapp", query, cache="populate")
    repeated, same_rows = runtime.run("myapp", query, cache="use")
    assert rows == same_rows
    print(repeated["cache_hit"], runtime.stats())
```

One native process can retain multiple projects under shared result-cache and
snapshot limits. Python output collections are additional memory. Stream results
with `collect=False, on_row=consumer` when the application does not need a second
copy of all returned bytes.

## Windows-specific review findings

The producer pins ancestor directory handles, rejects reparse-point traversal,
and opens captured source files without write sharing during a read. Windows
enumeration obtains full stat identity instead of relying on the incomplete
identity fields of `DirEntry.stat`. The catalog directory is excluded by file
identity so a case-only sibling on case-sensitive NTFS is not silently omitted.
These branches have simulated regression coverage and still require native
Windows execution, including case-sensitive directories and sharing violations.

Directory notification events are deliberately not used as proof of complete
change capture. A future watcher must reconcile after overflow or restart. The
USN journal can be an optional acceleration with journal-ID/cursor validation;
it is not a dependency of this release.

The publication protocol uses immutable generation directories and a replaced
CURRENT pointer. File/directory flush observations are recorded separately from
visibility. Power-loss durability depends on the OS, filesystem and storage
stack and has not been crash-tested here. An interruption after pointer replacement
may have committed; inspect CURRENT before retrying a guarded update.

Primary platform references are cataloged in
`research/sources_catalog_v0_11.json` and the review source addendum.
