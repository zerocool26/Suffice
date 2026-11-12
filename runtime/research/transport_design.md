# Bounded binary responses for exact artifact delivery

Research date: November 11, 2026, America/Chicago. Sources retrieved November 12 UTC. Baseline inspected: Suffice 0.12. This note defines design obligations and experimental boundaries for the next release. It does not claim that the proposed transport has already been implemented, validated, or measured. The final protocol specification and execution receipts must identify the completed subset.

## 1. Preserve the consumer's answer

The current service returns an ordered sequence of generation-local row IDs, relative paths, and complete original bodies. The Python API materializes each body as `bytes`. The protocol change should preserve this observation exactly while replacing hexadecimal row encoding with bounded raw-body frames.

Keep generation admission, Query identity, result-cache population strategy, worker retention, predicates, ranges, order, body hashes, and consumer collection mode fixed in each comparison. A binary full-body answer is comparable with a JSON/hex full-body answer. Hash-only output, content handles, deltas, batching, compression and deferred resolution are distinct changes and require separate comparisons.

The useful initial scope is one caller-serialized child process and its existing local stdin/stdout pipes. This does not introduce concurrent request multiplexing, shared-memory ownership, network authentication, or a new serializer for every internal object.

## 2. Primary-source lessons

Python's binary streams avoid text encoding and newline translation. Its raw stream interface permits short reads and writes; buffered methods have stronger effort-to-fill behavior, but framed parsing still needs explicit extent and EOF handling. A frame decoder should work when a producer splits bytes at any point. [T01]

Python `struct` defaults to native layout when no prefix is present. An external wire format should instead select a documented endianness, standard integer widths and no implicit native padding. A C++ producer must serialize those fields deliberately rather than writing an in-memory struct whose padding/layout depends on the toolchain. [T02]

Python subprocess pipes default to binary mode, but unmanaged pipe output can deadlock a child or parent. `communicate()` reads through process completion and buffers captured output; it is not a bounded streaming substitute for a persistent large-output service. Error cleanup must distinguish terminating a poisoned child from waiting for more unbounded diagnostic data. [T03]

On Windows, CRT text mode translates newlines and can treat Ctrl-Z as input EOF. `_setmode` with `_O_BINARY` suppresses text translation and should run before stream I/O. Merely using a Python binary pipe does not configure the native child's CRT stream mode. [T04, T05]

Arrow IPC is a useful framing precedent: explicit length/version metadata is parsed before the corresponding body, and its format specifies byte order. Arrow's array schema, alignment, dictionaries and zero-copy ownership contracts solve a broader problem than this row stream. The current project can apply the framing discipline without importing Arrow or claiming Arrow-style zero-copy access. [T06]

These sources support design constraints, not a measured Suffice speedup. No external benchmark number is transferred into this project.

## 3. Framing contract

The exact byte layout belongs in the implementation's versioned specification. The following properties should hold regardless of field widths chosen there:

| Element | Required property | Failure rule |
|---|---|---|
| Header | Fixed length; recognizable magic/version; known event kind; zero required reserved bits | Reject an unknown or malformed header |
| Sequence | Explicit fixed-width unsigned request sequence | Require the currently active request; reject disagreement with terminal metadata |
| Payload extent | One unambiguous interpretation of whether length includes header or only payload | Check the configured cap before allocating or requesting that extent |
| Row fields | Explicit row ID, path length, body length | Require exact partition of the row payload and checked arithmetic |
| Path | Exact existing UTF-8 bytes and path contract | Strict decode; reject invalid UTF-8 or prohibited NUL |
| Body | Arbitrary bytes, including every value from 0 to 255 | Never apply text decoding, newline conversion or sentinel scanning |
| Terminal event | Exactly one successful or native-error completion per request | Consume and validate the complete terminal frame before treating the connection as synchronized |

A useful validation order is fixed header, type/version/sequence, frame limit, fixed row prefix, nested extent checks, bounded path/body reads, then semantic delivery. Checking a nested length after allocating a frame of that size defeats the allocation bound.

For a frame with payload size L, fixed row prefix F, path size P and body size B, use subtraction-based checks:

`L >= F; P <= L-F; B == L-F-P`.

In native fixed-width arithmetic, computing `F+P+B` first can overflow before comparison. Python integers do not wrap, but their conversion into allocation sizes still requires a finite checked cap. Strict type checks also matter: Python booleans compare equal to integers, so accepting `True` as sequence 1 is avoidable schema ambiguity.

A receiver must implement exact reads without assuming one read returns a full header or body. Zero bytes before an expected extent completes is truncation. It must not reinterpret a shorter body as the beginning of a new frame. Magic scanning is not recovery: arbitrary file contents can contain the same magic bytes.

Keep four resource scopes distinct: per-frame bytes, cumulative requested body bytes, row count, and consumer retention. A small frame limit does not prevent a stream of many small frames from exhausting memory. Validate cumulative limits before delivering a row, and compare terminal row/byte accounting with what the receiver actually observed in full-output mode. Hash-only mode intentionally emits no row bodies and needs its own accounting expectations.

## 4. Streaming failures and synchronization

A completed native terminal error is recoverable when its sequence, schema and prefix accounting are valid. The next request begins after that fully consumed terminal frame. A parse failure, truncation, callback exception or transport write failure is different: the byte stream's remaining extent cannot safely be treated as a new request response.

The client should close/terminate its owned child after a fatal framing or callback failure. Preserve the distinction already present in 0.12 between a callback-raised `CatalogError` and an error parsed from a native terminal result. Exception class alone does not establish synchronization.

On the native side, once a row frame begins, a failed write can leave a partial frame. Appending a normally framed error afterward cannot make that earlier frame whole. Mark the writer failed and terminate rather than sending an apparently recoverable terminal response through the poisoned stream. This is also a reason to prepare all small row metadata before starting its output.

A privately constructed result cache remains transactional, but external row delivery remains streaming. A consumer may have received earlier complete rows before a later failure. Native delivered-prefix counters must be updated consistently, including successful row emission followed by a failure while constructing terminal metadata. A terminal failure must never claim zero delivered rows merely because the failure occurred outside the inner computation catch.

The 0.12 client currently calls blocking `stderr.read(8192)` after stdout EOF. A process can close stdout while remaining alive with stderr open; that diagnostic read can wait indefinitely. A fatal EOF path should first establish child termination or terminate the owned child, then obtain only bounded diagnostics. An unread stderr pipe also needs an explicit assumption or bounded drain strategy so it cannot fill and block ordinary progress. Do not merge stderr into the framed stdout channel.

Length bounds are not time bounds. A producer can announce a legal frame and stop sending without closing the pipe. Unless a tested deadline/cancellation mechanism is introduced, document that request duration is not bounded by framing limits. Forced process termination is cleanup for a failed owned service, not proof of graceful cancellation or transaction rollback.

## 5. Windows and portability

Put native stdin and stdout into binary mode before using them on Windows, check the mode-setting result, and retain normal diagnostic stderr separately. This avoids a split platform contract in which the producer's arbitrary output bytes are rewritten by the CRT. The commands may remain ASCII/LF encoded, but applying an explicit binary input mode makes their line behavior consistent as well.

Use fixed-width wire values without native struct casts. A header layout can be identical on GCC and MSVC even when their internal object layouts differ. Check conversions into stream or container sizes after the protocol caps are known.

A Linux test containing CR, LF, NUL, Ctrl-Z and all byte values exercises byte preservation in that environment. It does not establish native Windows behavior. Supply Windows-specific validation instructions and keep native Windows status unexecuted until receipts exist. Existing platform evidence should not be relabeled by adding conditional compilation alone.

## 6. Failure-focused validation matrix

The highest-value tests target protocol boundaries rather than duplicating the parser's branches:

| Case | Observable assertion |
|---|---|
| Headers and bodies split into one-byte fragments | Same complete ordered output after all fragments arrive |
| Empty output and selected empty body | Distinguish zero rows from one row with zero body bytes |
| All 256 byte values, CR/LF, NUL and Ctrl-Z in bodies | Exact original bytes reach the consumer |
| Non-ASCII path | Exact UTF-8 path reaches the consumer |
| Exact frame cap and cap plus one | Boundary accepted; excess rejected before payload allocation/read |
| Large declared length with no payload | Immediate cap rejection rather than enormous allocation or payload wait |
| Path/body extents that exceed, underfill or overflow the enclosing frame | Fatal rejection without callback |
| Wrong magic/version/type/reserved fields | Fatal framing rejection |
| Wrong sequence or frame/terminal sequence disagreement | Fatal rejection; no second request on that child |
| EOF at every fixed-field boundary and midway through path/body | Truncation detected; child cleaned up |
| Malformed terminal JSON, unknown status or impossible prefix totals | Fatal schema/accounting rejection |
| Normal native error followed by a valid request | Connection remains synchronized and useful |
| Callback failure, including a callback-raised `CatalogError` | Connection is closed and no abandoned rows contaminate another request |
| Same-client nested request | Reject before writing or changing the sequence |
| Child closes stdout but leaves stderr open | Fatal cleanup completes without waiting on diagnostic EOF |
| Client stops reading while native output is active | No recovery claim after a partial write; bounded cleanup where supported |

Run full integration against both transports so generation publication, old leases, cache hits, output caps and corruption checks retain their existing meaning. Inject malformed peers separately from valid-native tests. Freeze source and protocol identities before timing.

## 7. Measurement model

For one row with path P bytes, body B bytes, and JSON framing J bytes, the existing full row uses approximately `2P + 2B + J` wire bytes. A raw frame uses `P + B + H`, where H is its exact fixed framing/prefix overhead. For large rows the wire ratio can approach one half; small rows depend more on header constants. This is arithmetic about representation, not a latency prediction.

Count actual bytes read on the client side, including every row frame and terminal/control response. Report row bytes and whole-session response bytes separately if both are useful. A terminal object that describes its own wire size can create self-reference; measuring at the reader avoids that ambiguity. Decoded payload counters cannot stand in for transported bytes.

The main comparison should preserve the same process lifetime, complete corpus admissions, queries, body oracle, collection/callback behavior, cache and worker policy, instrumentation and shutdown. Use a fresh native process per arm, balanced paired order, and raw wall time alongside the established service measure. Keep audit work symmetric and explicitly identify what is subtracted; subtraction cannot undo cache or scheduling effects.

Useful secondary observations are complete consumer latency, first delivered row, total response bytes, and native/client resource measurements if separately instrumented. Native phase clocks overlap client wall and must not be added together. A no-body diagnostic mode can expose overhead but cannot serve as a faster full-output result.

Preserve all repeated observations and adverse cases. Do not select only large rows where raw framing has the largest representation advantage. Empty, small-row, mixed-size and cached-replay workloads reveal the constants and cost floors that a broad result needs.

## Sources

- **T01:** Python Software Foundation, [Python 3.12 `io` documentation](https://docs.python.org/3.12/library/io.html). Binary/text separation and raw versus buffered read/write semantics.
- **T02:** Python Software Foundation, [Python 3.12 `struct` documentation](https://docs.python.org/3.12/library/struct.html). Explicit byte order, standard sizes, padding and buffer parsing.
- **T03:** Python Software Foundation, [Python 3.12 `subprocess` documentation](https://docs.python.org/3.12/library/subprocess.html). Binary pipes, process lifetime, pipe deadlock and buffered communication limits.
- **T04:** Microsoft, [`_setmode`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmode?view=msvc-170). Configure translation mode before native stream operations.
- **T05:** Microsoft, [Translation mode constants](https://learn.microsoft.com/en-us/cpp/c-runtime-library/translation-mode-constants?view=msvc-170). CR/LF and Ctrl-Z translation in text mode; untranslated binary mode.
- **T06:** Apache Arrow, [Columnar format and IPC specification](https://arrow.apache.org/docs/format/Columnar.html). Framing/metadata/body separation as a precedent, with a broader array contract than the proposed Suffice transport.

No third-party papers or source packages were downloaded or redistributed for this note. Implementation-specific deductions and the proposed test/measurement design are project analysis, distinguished above from the cited platform/specification facts.
