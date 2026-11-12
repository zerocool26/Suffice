# Artifact capture and immutable publication — Suffice 0.11

The producer is [scripts/artifact_catalog.py](../scripts/artifact_catalog.py).
It captures actual files, derives features from the captured plaintext, writes
independent compressed payload rows, and publishes a complete generation through
one `CURRENT` switch. It never edits the source tree, prunes an existing
generation, or silently deletes a stale publisher lock.

## Invocation

After building the native core executable, run from the project root:

```sh
python3 scripts/artifact_catalog.py \
  --root /absolute/path/to/project \
  --catalog /absolute/path/to/artifact_catalog \
  --packer /absolute/path/to/build/suffice \
  --expected-current none
```

On native Windows use the corresponding paths and `suffice.exe`; PowerShell
uses a backtick rather than a backslash for line continuation, or enter one line.
Python 3.10+ and the `zstandard` package are required by the producer. The tested
Python interpreter/version and native packer hash are recorded in the test
results. The native reader does not use Python for query execution.

The optional `--expected-current` is `none` to require an empty catalog or the
exact 32-character lowercase hexadecimal current generation. A mismatch rejects
the request before source capture. Omitting it allows the producer to capture
against the current generation while holding the exclusive publisher lock.

Use `--exclude NAME` to add an excluded path-component name. Defaults skip
`.git`, `.hg`, `.svn`, `node_modules`, `target`, `build`, `dist`, `out`, `.venv`,
`venv`, `__pycache__`, `.mypy_cache`, `.pytest_cache`, `.ruff_cache`, `.next`,
`.nuxt`, `.turbo`, `.cache`, `CMakeFiles`, `bin`, and `obj`. These names apply at
every depth and are exact case-sensitive strings in the producer, even on a
case-insensitive filesystem. The catalog directory itself is excluded when it
is inside the source tree. The catalog cannot be the source root or its ancestor.
Catalog exclusion compares actual directory device/inode identity, so a distinct
case-sensitive sibling is preserved. A filesystem that supplies no usable
catalog inode identity is rejected. The root/ancestor guard may conservatively
reject ambiguous case variants on Windows; native Windows validation is pending.

## What a capture guarantees

1. Enumerate ordinary files and directories under one root without admitting
   symbolic links, junctions, other reparse points, devices, pipes, or sockets.
2. Record file identity, kind, length, modification time, and change/creation
   time. Read every admitted file, then compare its before/after file metadata
   and its expected enumeration metadata. Hash the complete captured plaintext.
3. Repeat the inventory after reading. For a new generation, repeat it again
   after packing and immediately before publication. A detected selected-file
   or directory change aborts publication.
4. Derive the four features from those exact captured bytes. The payload writer
   consumes the captured bytes, not a later re-open of the live source file.
5. Build a complete unpublished generation, validate native packing, flush the
   new files, rename the generation, and atomically replace `CURRENT` using a
   same-directory temporary pointer file.

The capture contract is **individually checked reads with a reconciled
inventory**. It is not a point-in-time filesystem transaction. A quiescent
source tree is required when the application needs a coherent project snapshot.
An adversarial writer that can manipulate files and metadata is outside this
capture protocol's consistency guarantee. A transactional source snapshot or a
cooperating build/editor freeze is a separate integration layer.

On POSIX, each path component is opened relative to an already opened directory
with `O_NOFOLLOW`; enumeration uses an opened directory descriptor. On Windows,
the implementation opens each ancestor with `FILE_FLAG_OPEN_REPARSE_POINT` and
directory semantics, rejects reparse attributes, and withholds delete sharing
while the chain is used. The file read also withholds write sharing. This branch
can reject an actively edited file with a sharing violation. **Native Windows
execution has not been validated in the Linux development environment.**
The POSIX final file open also uses `O_NONBLOCK` so replacement by a FIFO cannot
block the process before the regular-file check rejects it. Windows enumeration
uses full path stat under the pinned directory handles because `DirEntry.stat`
does not supply usable device/inode identity there.

Regular hard-linked files are admitted as separate paths and separately charged
logical bytes. Excluded names are not traversed or validated below their entry.
Paths preserve exact case and Unicode; the catalog does not claim portable
equivalence between different filesystems' name-comparison rules.

## Feature schema 1

Every raw feature row is four little-endian signed 32-bit integers:

| Channel | Meaning |
|---|---|
| 0 | Exact plaintext byte length |
| 1 | Extension class below |
| 2 | Number of `0x0A` bytes, not a language-aware line count |
| 3 | `1` if all bytes form strict UTF-8 and contain no NUL; otherwise `0` |

Empty content has length zero, zero LF bytes, and UTF-8 flag one. Binary files
are admitted; the flag records a fact rather than filtering them out.

| Class | Suffixes |
|---:|---|
| 0 | Other or no recognized extension |
| 1 | `.rs` |
| 2 | `.c`, `.h`, `.cc`, `.hh`, `.cpp`, `.hpp`, `.cxx`, `.hxx`, `.cu`, `.cuh` |
| 3 | `.js`, `.jsx`, `.ts`, `.tsx`, `.mjs`, `.cjs` |
| 4 | `.py`, `.pyi` |
| 5 | `.json`, `.jsonc`, `.toml`, `.yaml`, `.yml`, `.ini`, `.cfg`, `.xml` |
| 6 | `.md`, `.markdown`, `.rst` |
| 7 | `.glsl`, `.wgsl`, `.vert`, `.frag`, `.hlsl`, `.metal` |
| 8 | `.log`, `.txt` |

Suffix matching lowercases ASCII letters only. The final dot must occur after
the first character of the basename, so a file named `.json` belongs to class
zero. A future change to these semantics requires a new schema version; an
unchanged schema identifier is not permission to reinterpret old generations.

## Wire format and content identity

`CURRENT` is exactly `<32-lowercase-hex-generation>\n`. The directory is
`generations/<generation>/` and contains these four runtime components:

- `features.i32`: the interleaved raw feature rows.
- `features.sfp`: verified Suffice bit-plane features with 64-row physical blocks.
- `payload.spp`: one Zstandard frame per original file, with the inherited
  payload header/index/FNV checks.
- `rows.tsv`: row-to-path/content mapping.

`rows.tsv` has no header. Each line is exactly:

```text
<row> TAB <UTF8-path-as-lowercase-hex> TAB <decoded-size> TAB <SHA256> LF
```

Rows start at zero and increase consecutively. Paths are strictly increasing
by their UTF-8 byte sequences. They are relative POSIX paths, at most 4,096 UTF-8
bytes; empty, `.` and `..` segments, leading `/`, backslash, NUL, and colon are
rejected. Other Unicode and control characters are transported as hex and do
not become protocol delimiters. SHA256 is 64 lowercase hexadecimal characters.
All decimal fields are canonical unsigned decimal without redundant leading
zeroes. An empty catalog has an empty `rows.tsv`.

`manifest.txt` is strict ASCII in this exact order, with one final LF:

```text
SUFCAT01
generation<TAB><generation>
rows<TAB><row-count>
channels<TAB>4
schema<TAB>1
component<TAB>features.i32<TAB><bytes><TAB><SHA256>
component<TAB>features.sfp<TAB><bytes><TAB><SHA256>
component<TAB>payload.spp<TAB><bytes><TAB><SHA256>
component<TAB>rows.tsv<TAB><bytes><TAB><SHA256>
```

The literal `<TAB>` notation above denotes one tab, not text in the file.
`capture.json` is a separate producer provenance report; it does not change
runtime parsing semantics. Its packer command records original staging paths,
which disappear after rename; it is an invocation record, not a promise those
paths remain executable later. SHA256 detects changed contents under a trusted
manifest. It does not authenticate a publisher who can replace both data and
manifest. Native admission additionally verifies decoded content and feature
semantics against the row metadata.

The producer's source identity is SHA256 of
`b"SUFCAT-SCHEMA-1\0" + rows_tsv_bytes`. For no-change reuse it also compares
the newly derived raw feature bytes with the existing raw bytes. The random
generation name is an immutable publication identity, not a content hash.

## Reuse, updates, and disk growth

Every capture rehashes every selected source file. A same-size edit with a
restored modification time is therefore detected. An mtime-only touch leaves
the content generation unchanged. Prior component hashes and metadata are
verified before reuse.

If metadata and raw features match, the producer returns `status: unchanged` and
the same generation. It creates no generation and performs no payload
recompression. If any content/path changes, it writes a complete new generation
and recompresses every admitted file. It reports unchanged files, additions,
deletions, and content modifications; a rename is an addition plus a deletion.
There is no shared chunk store or incremental compression reuse in this release.

Default bounds are:

| Resource | Default |
|---|---:|
| Selected files | 100,000 |
| One plaintext file | 1 MiB |
| Total captured plaintext | 128 MiB |
| One completed generation | 512 MiB |
| Total logical catalog file lengths | 1 GiB |
| Retained generation directories | 16 |
| Row metadata | 16 MiB |
| Manifest / producer report | 16 KiB / 64 KiB |
| Path length | 4,096 UTF-8 bytes |
| Directory depth | 64 |
| Enumerated directories / entries | 100,000 / 1,000,000 |

The byte and generation-count caps reject a new publication rather than delete
old data. A no-change request remains allowed when the catalog is full, including
when a caller has deliberately reduced the byte cap below existing usage.
An existing over-budget catalog is reported; reuse does not repair that state.
Abandoned staging data and other regular files within the catalog are charged
to the logical catalog-byte count. The producer does not silently remove them.

Before building a candidate, the producer reserves space for the maximum
manifest/report plus one temporary pointer. This conservative reservation can
reject a very small configured budget even if the eventual report would be
smaller. The payload writer receives the remaining bounded capacity. The native
pack format's expected extent is checked. Budgets count logical file lengths,
not allocation-unit rounding, directory metadata, filesystem snapshots, or
space written concurrently by actors ignoring the publisher lock.

Captured plaintext remains in bounded Python-owned buffers until publication
finishes. Prior component verification, native packing, codec work, metadata,
and temporary copies incur additional memory. These are not process-RSS limits.
The persistent native catalog has separate in-memory ownership/cache budgets.

## Commit and failure semantics

The `.publish.lock` is created exclusively and contains a PID and random token.
Only its creator removes an unchanged matching lock. A surviving lock after a
crash requires inspection; PID reuse makes automatic stale-lock removal unsafe.

A failure before the visibility commit leaves the previous `CURRENT` unchanged.
Cleanup considers only this invocation's random staging or generation directory
and temporary pointer. An exception or signal can arrive after pointer
replacement succeeds but before Python records success. Cleanup therefore reads
`CURRENT` before deleting a final candidate and preserves it whenever the pointer
names that candidate or the pointer cannot be read. An interrupted caller must
inspect `CURRENT` before retrying; an exception alone does not prove that the
visibility commit failed. A crash or uncertain outcome can leave an unreferenced
complete generation or staging directory, subsequently charged to the disk budget.

The `CURRENT` replacement is the visibility commit. New readers see the old
or new pointer; the pointer names a completed generation. File flushing and,
where supported, directory flushing are attempted around the rename. A
post-commit directory-flush error is reported as a warning on a successful
publication; it must not be misreported as an uncommitted operation. Native
Windows directory fsync is not implemented, and power-loss durability has not
been crash-tested. Atomic visibility and storage durability are separate claims.

The returned JSON includes final elapsed time and publication/durability
observations. The immutable `capture.json` is written before commit, so it
records the candidate/provenance and policy, rather than the final pointer-flush
outcome. Do not treat that file alone as proof the generation became current.

## Validation and primary API references

Run the focused producer validation with the final native packer:

```sh
python3 tests/test_artifact_catalog.py \
  --packer /absolute/path/to/build/suffice \
  --json results/artifact_capture_validation.json
```

The tests use an independent plaintext/feature oracle, real native feature
packing, and real Zstandard decompression. They exercise empty/Unicode/binary
files, default exclusions, no-change and mtime-only reuse, updates and renames,
source mutations during capture, stale expectations, lock exclusion, link and
path rejection, corrupted prior data, output limits, disk saturation, orphan
accounting, pre-commit failure, post-commit interruption, uncertain cleanup, and
a post-commit flush failure. Check the saved
result's exact producer/test/packer hashes before transferring its coverage to
another source revision. Linux tests do not validate the Windows branch.

Primary API references informing the implementation:

- [Python `os` documentation](https://docs.python.org/3/library/os.html):
  descriptor-relative operations, `scandir`, `fsync`, and same-filesystem
  replacement semantics. The implementation uses Python 3.10+ interfaces;
  the current documentation version is not the tested interpreter version.
- [Microsoft `CreateFileW`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew):
  reparse-point opening, directory handles, and the lifetime of sharing modes.
  Withholding delete sharing constrains rename/delete access while the handle
  remains open. This supports the Windows design, not a claim that it was run.
