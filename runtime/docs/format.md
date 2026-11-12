# Suffice progressive storage format

The `.sfc` format stores signed 32-bit integers losslessly in independently
addressable significance planes. It is a progressive representation: a consumer
can read the most significant planes and request finer planes later. It does not
compress arbitrary input information. Fully reading every plane costs essentially
the same payload bytes as raw int32, plus metadata and final-block padding.

All header fields and raw input integers use little-endian byte order. Signed
integers use their two's-complement representation. Bit order inside a plane byte
is specified separately below.

## Raw input

Raw input is a headerless sequence of interleaved int32 rows. For `C` channels,
the input order is:

```text
row0.channel0, row0.channel1, ..., row0.channel(C-1),
row1.channel0, row1.channel1, ..., row1.channel(C-1), ...
```

The caller supplies `C`, which must be between 1 and 1024. Input length must be
divisible by `4*C`. An empty input is valid. The source must remain immutable
while mapped or packed.

## Version 1 header

The fixed header occupies 64 bytes. It is followed by one eight-byte range record
per channel. `metadata_bytes` is therefore exactly `64 + 8*C`.

| Byte offset | Width | Encoding | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | ASCII | Exact magic `SUFFICE1` |
| 8 | 4 | uint32 LE | Format version, currently 1 |
| 12 | 4 | uint32 LE | Total metadata bytes |
| 16 | 8 | uint64 LE | Number of rows |
| 24 | 4 | uint32 LE | Number of channels |
| 28 | 4 | uint32 LE | Rows per full block |
| 32 | 8 | uint64 LE | Payload bytes |
| 40 | 8 | uint64 LE | Payload FNV-1a checksum |
| 48 | 8 | uint64 LE | Metadata FNV-1a checksum |
| 56 | 8 | uint64 LE | Reserved; must be zero |
| 64 + 8*c | 4 | int32 LE | Exact minimum for channel c |
| 68 + 8*c | 4 | int32 LE | Exact maximum for channel c |

`block_rows` is a power of two from 32 through 65536. The default packer uses
64. The final block may contain fewer rows. Empty input has no blocks and uses
minimum and maximum zero for every channel.

The parser rejects unknown versions, nonzero reserved fields, invalid dimensions,
invalid ranges, overflowed sizes, incorrect metadata checksums, truncation, and
trailing bytes.

## Payload order

The payload is ordered by block, then channel, then significance plane:

```text
block 0:
  channel 0: plane 0, plane 1, ..., plane 31
  channel 1: plane 0, plane 1, ..., plane 31
  ...
block 1:
  ...
```

For each signed input value `v`, first form:

```cpp
uint32_t biased = std::bit_cast<uint32_t>(v) ^ 0x80000000u;
```

Plane zero contains bit 31 of `biased`. Plane 31 contains bit zero. For a block
with `R` valid rows, every plane has `P = ceil(R/8)` bytes. Row `r` is bit
`r % 8` of byte `r / 8`: row zero occupies the least significant bit. Unused bits
in the last plane byte are zero.

Let `B` be the configured full block size, `C` the channel count, and `H` the
metadata length. For block `b`, channel `c`, and plane `k`, its byte offset is:

```text
H + b * C * 32 * (B/8) + (c*32 + k) * ceil(valid_rows(b)/8)
```

All blocks preceding the final block are full, so this formula requires no
per-block offset table. The payload size is:

```text
32*C * (floor(rows/B)*(B/8) + ceil((rows % B)/8))
```

## Progressive interval meaning

After reading the first `k` planes, place the known bits in their original
positions and leave the unknown suffix zero. Call that uint32 value `prefix`.
Using int64 arithmetic, the exact signed value lies in:

```text
lower = int64(prefix) - 2147483648
upper = lower + (2^(32-k) - 1)
```

At `k=32`, the added width is zero. At `k=0`, the interval covers the entire
int32 range. A verified channel range may tighten either endpoint. The format
does not impose an empirical or probabilistic error model: these intervals
contain the represented value deterministically.

## Checksums and validation

Checksums use 64-bit FNV-1a: start with `14695981039346656037`; for each byte `b`,
replace the accumulator with `(accumulator xor b) * 1099511628211`, modulo
2^64. The payload checksum covers all payload bytes, including padding. The
metadata checksum covers the entire metadata region, treating bytes 48–55 as
zero while hashing.

These checksums detect accidental damage. They do not authenticate hostile data.

`PackedDataset` construction validates the header, metadata checksum, and exact
file length without scanning the payload. `verify_payload()` is an explicit
full pass that verifies the payload checksum, zero padding, and exact channel
minima/maxima. Perform this validation before trusting declared ranges from a new
file for exact computations. Keep validation separate from timed demand execution
and account for its preparation cost. The file must remain immutable afterwards.

## API

`RawDataset(path, channels)` maps an input and exposes its dimensions, individual
values, raw byte span, and interleaved `read_rows()` operation.

`PackedDataset(path)` maps a packed input. `block_count()`, `block_length()`, and
`plane_bytes()` expose its geometry. `plane_view(block, channel, first_bit,
count_bits)` returns a nonowning span over contiguous planes. `read_planes()`
copies that same span into caller-provided storage. `decode_block()` reconstructs
one channel's values into caller-provided int32 storage.

Plane access, plane copying, and block decoding do not allocate memory. Their
spans remain valid while the dataset's mapping remains alive. Full validation
uses bounded scratch buffers and is not part of the allocation-free hot path.

`pack_file(raw, destination, channels, block_rows)` produces a completed file
using scratch proportional to the block size and channel count. It maps the raw
file rather than allocating a full decoded copy. Packing reads every input value
and writes every encoded value. It returns `DatasetInfo` with exact ranges and
file dimensions.

## File handling and accounting

The implementation uses read-only POSIX mappings on Linux and read-only Windows
file mappings through wide-character APIs on Windows. A mapping's virtual size
is not a measure of resident working memory. The OS may satisfy accesses from its
file cache.

The packer writes an exclusively created temporary file beside its destination,
flushes it, and atomically installs the completed file without replacing an
existing destination. POSIX installation uses a hard link followed by temporary
cleanup; Windows uses a non-replacing move. Input/output identity and existing
destinations are rejected. This provides complete-file visibility; no stronger
power-loss guarantee for directory metadata is claimed.

The number of bytes in returned plane spans is an encoded-payload access count.
It is not a measurement of physical disk, RAM, PCIe, or GPU traffic. Cache-line
and page granularity can move more bytes than a consumer explicitly requests.
