#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace suffice {

struct ChannelRange {
    std::int32_t min{};
    std::int32_t max{};
};

struct DatasetInfo {
    std::uint64_t rows{};
    std::uint32_t channels{};
    std::uint32_t block_rows{};
    std::uint64_t metadata_bytes{};
    std::uint64_t file_bytes{};
    std::vector<ChannelRange> ranges;
};

class MappedFile;

// Input consists of interleaved, little-endian signed int32 values.
// A mapping reserves virtual address space; it does not imply all bytes are resident.
// copy_bytes instead owns an independent immutable byte copy. Source storage must
// remain valid and unmodified during the copy; it can be changed or freed afterward.
class RawDataset {
public:
    explicit RawDataset(const std::filesystem::path& path, std::uint32_t channels);
    [[nodiscard]] static RawDataset copy_bytes(std::span<const std::uint8_t> bytes,
                                               std::uint32_t channels);
    ~RawDataset();
    RawDataset(RawDataset&&) noexcept;
    RawDataset& operator=(RawDataset&&) noexcept;
    RawDataset(const RawDataset&) = delete;
    RawDataset& operator=(const RawDataset&) = delete;

    [[nodiscard]] const DatasetInfo& info() const noexcept { return info_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::int32_t value(std::uint64_t row, std::uint32_t channel) const;
    void read_rows(std::uint64_t first, std::uint64_t count,
                   std::span<std::int32_t> output) const;

private:
    RawDataset(std::unique_ptr<MappedFile> file, DatasetInfo info) noexcept;
    std::unique_ptr<MappedFile> file_;
    DatasetInfo info_;
};

class PackedDataset {
public:
    explicit PackedDataset(const std::filesystem::path& path);
    // Copy the complete encoded bytes, then validate the owned header/metadata.
    // Source storage must stay valid and unmodified until this call returns.
    // Payload verification remains a separate full pass, as for mapped input.
    [[nodiscard]] static PackedDataset copy_bytes(std::span<const std::uint8_t> bytes);
    ~PackedDataset();
    PackedDataset(PackedDataset&&) noexcept;
    PackedDataset& operator=(PackedDataset&&) noexcept;
    PackedDataset(const PackedDataset&) = delete;
    PackedDataset& operator=(const PackedDataset&) = delete;

    [[nodiscard]] const DatasetInfo& info() const noexcept { return info_; }
    [[nodiscard]] std::uint64_t block_count() const noexcept;
    [[nodiscard]] std::uint32_t block_length(std::uint64_t block) const;
    [[nodiscard]] std::size_t plane_bytes(std::uint64_t block) const;

    // Planes are MSB first after uint32(value) xor 0x80000000.
    // Within a plane byte, row 0 occupies bit 0 (LSB).
    // These methods allocate nothing. Borrowed views remain valid while their
    // backing owner lives; moving the dataset transfers that owner without
    // relocating its bytes. Destruction/reassignment of that owner ends the views.
    [[nodiscard]] std::span<const std::uint8_t> plane_view(
        std::uint64_t block, std::uint32_t channel,
        std::uint32_t first_bit, std::uint32_t count_bits) const;
    std::size_t read_planes(std::uint64_t block, std::uint32_t channel,
                            std::uint32_t first_bit, std::uint32_t count_bits,
                            std::span<std::uint8_t> output) const;
    void decode_block(std::uint64_t block, std::uint32_t channel,
                      std::span<std::int32_t> output) const;

    // Full data pass, separate from construction and demand execution.
    // Verifies payload checksum, unused padding bits, and exact per-channel ranges.
    // Checksums detect accidental damage, not hostile modifications.
    void verify_payload() const;

private:
    explicit PackedDataset(std::unique_ptr<MappedFile> file);
    std::unique_ptr<MappedFile> file_;
    DatasetInfo info_;
    std::uint64_t payload_checksum_{};
};

// Uses bounded scratch memory and installs a completed file atomically.
// Refuses to overwrite an existing destination. block_rows must be a power of two
// in [32, 65536]; channels must be in [1, 1024]. Empty input is permitted.
[[nodiscard]] DatasetInfo pack_file(const std::filesystem::path& raw,
                                    const std::filesystem::path& destination,
                                    std::uint32_t channels,
                                    std::uint32_t block_rows = 64);

} // namespace suffice
