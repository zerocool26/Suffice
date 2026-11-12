# Independent catalog source review

Scope: read-only review of `apps/catalog/generation.cpp`, `digest.hpp`, `catalog.cpp`, `main.cpp`, and the two public catalog headers in the 0.11 working tree. `review_source_hashes.json` records the reviewed-stage identities. This review is separate from the implementation owner's native correctness and integration suites. Later changes are not automatically covered by these identities.

## Result

No wrong-output, stale-publication, or retained-owner accounting defect was found in the reviewed runtime under its documented caller-serialized use. One concrete metadata-parser resource issue was reported and corrected by the root agent. Two API scope distinctions were clarified in the public header, and an admission preflight was added. The measurement/accounting distinctions below should remain explicit in the release documentation.

This is a source review with a focused independent digest/UTF-8 execution probe, not a formal proof, hostile-filesystem audit, native Windows test, or performance claim.

## R1: metadata parsing amplified temporary memory before rejecting malformed data — corrected

The first loader version split the entire `rows.tsv` into a dynamic vector of string views, then checked the declared row count. A metadata component consisting largely of newline delimiters could allocate approximately 16 bytes of views per input byte before the count mismatch was rejected. It also reserved the declared artifact count before proving that the metadata could contain that many rows, and decoded a hex path before enforcing its decoded-byte cap.

The revised loader now:

- Parses exactly the declared number of rows sequentially and rejects trailing rows.
- Parses each row into a fixed four-field array.
- Rejects counts exceeding the metadata's minimum 72 bytes per row before reserving the output vector.
- Checks the hex field's parity and decoded-size bound before allocation.

The 72-byte bound is conservative and correct: the shortest canonical row has a one-digit row index, three tabs, a two-digit hex path, a one-digit size, a 64-digit SHA-256, and LF. Other remaining dynamic splits operate on the 4 KiB manifest, bounded paths, or constant extension groups. The loader still makes input copies and an output metadata vector; its configured component-byte limits are not a peak-RSS guarantee.

## Runtime ownership and transaction reasoning

### Publication

`publish` validates the expected revision, full-body validation flag, metadata cardinality, and budgets before changing the current project. It creates the candidate privately, reserves the tracking vectors, and stages a new map node before publishing. Existing-entry publication uses nonallocating ownership assignment. The global revision checks overflow rather than wrapping. Project tombstones preserve revision history across a present/absent transition, so an old expected revision cannot recreate a deleted generation accidentally.

The conservative old-plus-new admission policy is visible and useful: a publication can be rejected even when releasing the old unpinned generation afterward would have made enough space. This favors a straightforward failure-preservation guarantee over temporarily exceeding the admitted budget.

### Pinned and aliased storage

Snapshot charges use weak ownership and are counted while the admitted snapshot remains alive, including an external retained snapshot owner. Multiple generations sharing the same live snapshot pointer are charged once. Metadata charges follow live generation owners, including retired leases. Retiring the current project entry does not falsely claim reclamation while a reader still owns the generation.

The lease does not expose its internal Query or Snapshot owner. Results remain privately owned by the catalog; result replay does not return an independently retained cache handle. These restrictions make the stated retained-result budget tractable. Construction buffers, compiled plans, worker allocations, allocator bookkeeping, and consumer memory are outside the advertised snapshot/result byte pools. Entry-count and source-length caps provide additional bounds but do not establish a single hard process-memory cap.

### Exact keys and invalidation

The result key includes immutable generation identity, exact program source bytes, and range. A new generation therefore cannot accidentally reuse an old result even when row count and program text are unchanged. Plan identity follows the same generation/source association. Publication and drop invalidate stored results/plans for the affected project; explicitly retained old leases can still execute exact queries against their old immutable owners.

A callback cannot remove catalog entries while replay is using them because same-catalog reentry is rejected. The normal run also retains its own generation owner if a callback overwrites the caller's lease variable. Results and plans are allocated/staged before victim eviction, and optional failed cache construction preserves output already successfully delivered.

## R2: public publication is a narrower validation boundary than the file loader — clarified

The reviewed `load_generation` checks UTF-8 path encoding, component SHA-256, row/body SHA-256, and re-derives the four feature semantics from decoded bodies. The reviewed public `Catalog::publish` checks relative path shape, order, digest syntax, body size, and the Snapshot's full validation flag. It does not independently verify UTF-8, SHA-256-to-body binding, or the four-feature extractor contract.

The revised public header now explicitly assigns UTF-8, SHA-256-to-body agreement, and external feature extraction semantics to the caller/loader. This resolves the contract ambiguity. The disk-loader route supplies the stronger checks; direct callers remain responsible for them. Do not attribute the loader's complete validation to every public publish call without stating this distinction.

This is a contract finding, not an observed failure of the disk-loader route. A separately supplied full Snapshot still preserves its own payload correctness contract.

## R3: a cache hit uses the backend only as an enum value — clarified

On a cache hit, the reviewed runtime checks that the backend enumerator is valid, then replays the exact cached bytes. It does not check whether forced Dense or Wordcode lowering would be available for the program. On a cache miss, the Worker performs that availability check.

The revised `RunOptions` comment explicitly defines backend selection for computed misses and bypass calls; complete cache hits are independent of that choice. This resolves the ambiguity. The immutable `limits()` accessor is also explicitly exempted from the blanket same-catalog reentrancy rejection rule.

## R4: Populate changes the session break-even model

The reviewed Populate API first delivers a normal successful run and then performs two additional passes to construct a cache. For `K` total requests, where the first request populates and the rest hit, the relevant comparison is:

`T_direct + C_build + (K - 1) * T_replay <= K * T_direct`.

For a positive saving per replay, the modeled threshold in total requests is:

`K >= 1 + ceil(C_build / (T_direct - T_replay))`.

The old 0.10 expression compared construction plus `K` replays against `K` uncached calls. The old median three/four-replay estimates must not be copied as the new API's total-request break-even. A complete session experiment should include all normal/populate calls, construction, invalidation, misses, and evictions.

## R5: admission telemetry and configured retention limits have distinct scopes

The first reviewed CLI `OPEN` used default admission limits before the separately configured catalog retention limits. After this was reported, the CLI was changed to pass remaining snapshot headroom and remaining metadata headroom into admission. The loader now compares declared snapshot component bytes against available snapshot headroom before reading the components. Full admission still checks additional block-bound storage; metadata input bytes and retained metadata capacities remain distinct quantities. These changes avoid clearly impossible captures early without claiming a peak-RSS cap.

The revised runtime also reports whether a lease is retired and executes retired leases without retaining new plans/results. This prevents old-reader activity from rebuilding an internal retention chain after publication invalidated the project's old cache entries. Exact old-generation output remains supported.

`verified_body_bytes` counts the full decoded size accumulated in the feature-verification pass. The loader also requests Snapshot Full validation first, which already decompresses each body once. The field is a logical byte count for the additional verification pass, not total decoder work across admission. `component_bytes` similarly names component contents, not every physical I/O byte or filesystem metadata operation.

## Independent SHA-256 and UTF-8 verification

The copied, frozen digest header is `digest_review_snapshot.hpp`. Its SHA-256 is recorded in `digest_review_result.json`. The probe passed:

- 480 SHA-256 comparisons against Python `hashlib.sha256`.
- The two official NIST example messages, covering one- and two-block processing.
- Padding-boundary lengths 0 through 131 and longer inputs up to 1,000,000 bytes.
- 480 UTF-8 acceptance comparisons against Python's strict UTF-8 decoder.
- Explicit malformed continuation, overlong, surrogate, truncation, and above-U+10FFFF encodings, plus valid boundary scalars.

The probe compiled under GCC 14.2 with C++20, optimization, and warnings as errors. The production digest header still matched the frozen copy at the end of the original probe. `verify_digest.py` reproduces the cases against the frozen source without overwriting the retained evidence. Counts overlap: this is 480 messages checked for two properties, not 960 independent inputs. These checks are not a FIPS certification or proof of all possible message lengths.

Primary NIST vector source: [Secure Hash Algorithm — Message Digest Length = 256](https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/SHA256.pdf).

## Remaining review boundaries

The generation reader explicitly assumes trusted local storage. Path type checks followed by stream opens do not provide a race-free defense against an adversarial filesystem, and this review does not claim one. Concurrent publishers must preserve immutable generation directories; a directory being deleted or changed during read can cause rejection. A caught read/admission failure leaves the currently admitted in-memory generation intact.

Native Windows execution, crash/power-loss persistence behavior, Python capture/client implementation, complete session output equivalence, and the implementation owner's broader tests are outside this focused review. They need their own retained evidence and should not be inferred from the SHA-256 probe or source inspection.


## Follow-up review: native protocol, Python client, and capture/publication

Recorded at **2026-11-11T20:46:13.238398-06:00** (America/Chicago), **2026-11-12T02:46:13.238398+00:00** (UTC). The earlier review is preserved above. This section expands the previous Python-capture/client boundary; the original digest probe remains a separate execution experiment.

The files inspected after the corrections are identified in `review_followup_source_hashes.json`:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `apps/catalog/main.cpp` | 17203 | `2d486590d1bdea09ffc92da344d57686404dec4cb4e8cfaf76fc13c2db054f28` |
| `scripts/catalog_client.py` | 9239 | `f114524d5b35f2f8bac6fbae27225605f837e4ca0fc960e97ea6ae1d11163a42` |
| `scripts/artifact_catalog.py` | 36582 | `2efab4088044bf119a1a53a4b2339e3ffacd4e0d147c726d74b18b94fef705ca` |

**Result:** six concrete follow-up findings were reported and corrected by the owning implementation agents. No remaining blocking defect was found in these files under the documented trusted-local-store, cooperating-publisher, and serialized-client assumptions. This statement is a source-review conclusion, not a proof of all interruption points or native Windows filesystem behavior.

### F1: a callback exception could leave the client stream desynchronized — corrected

The original client caught every `CatalogError` as though it represented a fully consumed terminal native error. A row callback can raise that same public exception type before the native request has finished. In that case, unread rows and the terminal response remained in the pipe, and the next request consumed the wrong sequence.

The client now distinguishes a native terminal error using an explicit flag. Callback exceptions of this type force-close the child, as do other interrupted/malformed request paths. A native terminal error leaves the healthy child available. The new active-request guard runs before sequence mutation or wire output, so a nested callback request cannot steal the outer request's response. The `finally` reset makes the guard consistent on success and failure. This guard establishes reentrancy behavior; it is not a multithreaded synchronization primitive.

### F2: Windows directory-entry identity could reject unchanged files — corrected

The inventory originally retained `DirEntry.stat()` values and later compared them with metadata from the opened file handle. Python explicitly documents that Windows `DirEntry.stat()` zeroes `st_ino`, `st_dev`, and `st_nlink`. Comparing those placeholder identity values with full file information can reject a normal unchanged file.

The inventory and catalog disk walk now obtain full path stat information on Windows while the ancestor-directory handles remain held. The POSIX directory-entry route remains available. The implementation owner's regression exercises the Windows metadata-selection branch using a simulated directory entry; it does not constitute native Windows execution.

Primary source: [Python `os.DirEntry.stat`](https://docs.python.org/3/library/os.html#os.DirEntry.stat).

### F3: the persisted report overclaimed directory flush support — corrected

The original capture report wrote `directory_fsync_supported=True` on every non-Windows host before testing whether directory flushes were supported. Some POSIX filesystems can reject directory `fsync` with a recognized unsupported-operation error. Only the returned in-memory result was later corrected, leaving the immutable report inaccurate.

The precommit `capture.json` now records this observation as `null`. The returned result can report the actual stage, generation-parent, and catalog-directory flush outcomes after the operations occur. A postcommit flush error is reported separately; it must not trigger deletion of a committed generation. Power-loss durability remains dependent on the actual filesystem, operating system, and storage stack.

### F4: Windows path equality could silently exclude a distinct source sibling — corrected

The original exclusion used `child == catalog`. `WindowsPath` follows `PureWindowsPath` case-folding equality, including when the actual Windows directory permits distinct case-only names. A store named `Catalog` and a distinct source sibling named `catalog` could therefore both be skipped.

The capture now identifies the actual catalog directory using its device/inode identity and rejects a filesystem that does not expose the required catalog identity. It no longer uses case-folded path equality to omit source entries. The implementation owner retained an actual Linux case-distinct sibling capture regression; Windows support remains subject to native execution. The separate initial ancestor-overlap check is conservative on case-sensitive Windows paths and can reject an otherwise distinct source/store pair; that behavior fails closed rather than publishing an incomplete inventory.

Primary sources: [Python pathlib general properties](https://docs.python.org/3/library/pathlib.html#general-properties) and [Microsoft Windows directory case sensitivity](https://learn.microsoft.com/en-us/windows/wsl/case-sensitivity).

### F5: a raced FIFO replacement could block before file-kind validation — corrected

POSIX final-component open originally used `O_RDONLY | O_NOFOLLOW` before `fstat` established that the object was an ordinary file. Replacing an inventoried file with a FIFO could block in `open`, waiting for a writer, so the file-kind check was never reached.

The final open now also uses `O_NONBLOCK`; the subsequent `fstat` rejects the FIFO before reading it. The implementation owner's regression uses an actual POSIX FIFO and a subprocess timeout, providing execution evidence for the hang prevention on the tested Linux host. Regular-file capture semantics are unchanged.

### F6: interruption after pointer replacement could delete the published generation — corrected

The original `finally` logic deleted the candidate whenever the Python variable `committed` was false. A signal or exception can arrive after `os.replace(pointer_temp, CURRENT)` has changed the pointer but before Python executes `committed=True`. Cleanup could then delete the directory already named by `CURRENT`, leaving a dangling publication.

Cleanup now rereads `CURRENT` before deleting an apparently uncommitted final directory. It removes the candidate only after establishing that the pointer does not name that candidate. If reading the pointer is uncertain, cleanup conservatively retains the candidate; the normal disk inventory continues to charge any retained orphan. It also tolerates a stage rename that completed immediately before Python recorded that completion.

The implementation owner's focused fault injection calls the real pointer replacement and then raises `KeyboardInterrupt`, testing the dangerous post-syscall window. A second case makes the cleanup pointer read fail and checks that the uncertain candidate survives. These cases are stronger evidence than merely raising before replacement. They are still not an exhaustive signal-interleaving or power-failure test.

### Additional protocol, resource, and publication checks

The native protocol stores at most 256 KiB of input before draining/rejecting an overlong line. Field parsing limits its vector to nine entries before growth; text fields validate byte caps before hex decoding. Output hex conversion uses a bounded stack buffer. Per-request output row/decoded-byte limits and the client response-line cap bound the advertised output quantities. They do not account for all consumer object overhead or promise a total Python-process memory ceiling.

The Python client accepts native sequence-checked row events followed by one terminal result and closes on malformed or interrupted exchanges. Explicitly pinned leases have their own count limit. Native `OPEN` preflights admission against remaining snapshot and metadata headroom; publication and retired-owner semantics remain as reviewed above.

The capture checks source inventory before and after reads and again before publication. Each read checks the opened file's identity/metadata and length. This supports individually stable reads plus reconciled membership; it does not establish a filesystem-wide transaction when the source is changing. The capture report correctly requires a quiescent source for coherent project-generation meaning.

Generation construction reserves metadata/report and pointer-temporary bytes, enforces the candidate budget, and checks the completed generation before pointer replacement. The retained-store walk charges ordinary files in old generations and abandoned staging directories. No automatic deletion of retained historical generations is introduced. These are logical file-length budgets, excluding filesystem allocation overhead, external writers, and process construction buffers. Rejection preserves previously published generations; uncertain cleanup may retain a charged orphan.

The actual publication sequence prepares and flushes component files, flushes the stage directory where supported, moves the immutable candidate into the generations directory, writes and flushes the temporary pointer, and replaces `CURRENT`. Same-volume replacement visibility and successful directory flushes must not be expanded into a universal power-loss durability claim. Python documents successful rename atomicity as a POSIX requirement; the Microsoft replacement APIs document their own flags and failure behavior. Native Windows validation is still required.

Primary references: [Python `os.replace`](https://docs.python.org/3/library/os.html#os.replace), [Microsoft MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw), and [Microsoft ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew). These additional sources are cataloged separately in `sources_review_addendum_v0_11.json`; they do not alter the original 18-source design catalog.

### Execution evidence and attribution

This reviewer inspected the implementation owner's retained `results/artifact_ingest_v0_11/publication_closed_result.json` and confirmed that its recorded producer SHA-256 matches the producer in this follow-up hash record. It reports **22 passing producer test cases**, **82 complete plaintext rows / 4,356 bytes compared**, one preserved interrupted commit, and one retained uncertain candidate. The record explicitly states that native Windows was **not** tested. These are implementation-owner tests, distinct from this reviewer's independent 480-message digest/UTF-8 probe.

The owner also reported passing Release and sanitized native/client integration suites, including fourteen corrupted-generation rejection cases. Consult the corresponding integration records for their own source/binary identities and exact case definitions; this source review does not replace them. No throughput, session break-even, or whole-project performance claim follows from these correctness tests.
