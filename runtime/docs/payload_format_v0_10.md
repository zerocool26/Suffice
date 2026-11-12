# Suffice payload store 1

This optional application format does not alter the existing packed feature format. All integers are little-endian. Row order must match the raw and packed feature records. Each payload is one ordinary Zstandard frame with a declared content size. Skippable frames, concatenated frames, trailing bytes and unknown-size frames are rejected.

## Header: 64 bytes

| Offset | Field | Type |
|---:|---|---|
| 0 | Magic SUFPAY01 | 8 bytes |
| 8 | Version = 1 | uint32 |
| 12 | Codec = 1 (Zstandard) | uint32 |
| 16 | Number of rows | uint64 |
| 24 | Feature channels | uint32 |
| 28 | Index entry size = 24 | uint32 |
| 32 | FNV-1a-64 of raw feature bytes | uint64 |
| 40 | Index length = rows × 24 | uint64 |
| 48 | Complete store length | uint64 |
| 56 | FNV-1a-64 of header with this field zeroed, then index | uint64 |

FNV-1a uses offset14695981039346656037 and prime1099511628211 with arithmetic modulo2^64. These checks detect accidental corruption and file mixing; they are not adversarial authentication. Snapshot/query/cache identity inside the runtime uses ownership, not these hashes.

## Index entry: 24 bytes per row

| Offset | Field | Type |
|---:|---|---|
| 0 | Absolute frame offset | uint64 |
| 8 | Compressed frame length | uint32 |
| 12 | Decoded payload length | uint32 |
| 16 | FNV-1a-64 of decoded bytes | uint64 |

Frames must be contiguous in row order, starting immediately after the index and ending exactly at the recorded file length. The implementation checks arithmetic and extents before reading frame headers. There are no null payloads; empty byte strings are valid.

## Admission and execution

The default Full policy copies all input bytes, verifies packed metadata/checksum/padding/ranges, compares every raw/packed feature value, derives conservative physical-block ranges, checks frame structure, and decodes/verifies every payload once before publishing an immutable snapshot. Admission cost includes this full decoding pass.

OnDemand is explicit: frame structure and feature facts are checked at admission, but complete frame content and its decoded FNV are checked when selected. An unselected body is not claimed verified. Selected-frame errors propagate; previously delivered sink rows may remain.

After Full admission, immutable frame bytes retain their verified meaning. Repeated decoding still checks codec errors, native frame checksums and exact decoded length; it skips the redundant additional FNV content pass. One reusable Zstandard context and one bounded row buffer belong to each worker. ZSTD_sizeof_DCtx reports current context allocation, not allocator overhead, future peak or process RSS.

The primitive result is the increasing sequence of (global row ID, complete decoded bytes). Sink spans are valid only during the callback. Finite output limits are checked against the exact selection before the first callback. Consumer exceptions abort with a delivered prefix. Result-cache construction publishes no partial object; replay requires the identical owning query and exact range.

## Plaintext reference format

The experiment-only reference begins with SUFREF01 and uint64 row count. It stores uint32 length followed by original plaintext bytes for each row. It is produced from source bytes before compression. Complete candidate outputs are compared byte-for-byte with this independent plaintext source. It is not a production payload codec.
