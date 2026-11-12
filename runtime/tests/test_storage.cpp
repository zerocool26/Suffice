#include "suffice/storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("storage test: ") + message);
}

template<class F> void must_throw(F&& function, const char* message) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-storage-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("could not create test directory");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

void put_u32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) value = (value ^ byte) * 1099511628211ULL;
    return value;
}
void refresh_metadata(std::vector<std::uint8_t>& bytes, std::size_t length) {
    std::fill(bytes.begin() + 48, bytes.begin() + 56, std::uint8_t{0});
    put_u64(bytes.data() + 48, fnv(std::span<const std::uint8_t>(bytes.data(), length)));
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("write test fixture failed");
}
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (length < 0) throw std::runtime_error("read test fixture length failed");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file && !bytes.empty()) throw std::runtime_error("read test fixture failed");
    return bytes;
}
void write_values(const std::filesystem::path& path, std::span<const std::int32_t> values) {
    std::vector<std::uint8_t> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i) put_u32(bytes.data() + i * 4, std::bit_cast<std::uint32_t>(values[i]));
    write_bytes(path, bytes);
}

void check_roundtrip(std::uint32_t rows, std::uint32_t channels, std::uint32_t block_rows) {
    TempDirectory directory;
    const auto raw_path = directory.path / "input.i32";
    const auto packed_path = directory.path / "output.sfc";
    std::vector<std::int32_t> values(static_cast<std::size_t>(rows) * channels);
    std::uint32_t random = 0x834712abU;
    for (auto& value : values) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        value = std::bit_cast<std::int32_t>(random);
    }
    if (!values.empty()) values.front() = std::numeric_limits<std::int32_t>::min();
    if (values.size() > 1) values.back() = std::numeric_limits<std::int32_t>::max();
    write_values(raw_path, values);
    suffice::RawDataset raw(raw_path, channels);
    require(raw.info().rows == rows && raw.info().channels == channels, "raw dimensions");
    require(raw.bytes().size() == values.size() * 4, "raw byte length");
    std::vector<std::int32_t> copied(values.size());
    raw.read_rows(0, rows, copied);
    require(copied == values, "interleaved raw row read");
    const auto written = suffice::pack_file(raw_path, packed_path, channels, block_rows);
    suffice::PackedDataset packed(packed_path);
    require(written.file_bytes == std::filesystem::file_size(packed_path), "reported file bytes");
    require(packed.info().rows == rows && packed.info().channels == channels, "packed dimensions");
    require(packed.info().block_rows == block_rows, "packed block rows");
    packed.verify_payload();
    std::vector<std::int32_t> decoded(block_rows);
    std::vector<std::uint8_t> copied_planes(static_cast<std::size_t>(block_rows / 8) * 32);
    for (std::uint64_t block = 0; block < packed.block_count(); ++block) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            packed.decode_block(block, channel, decoded);
            const auto rows_here = packed.block_length(block);
            for (std::uint32_t row = 0; row < rows_here; ++row) {
                const auto index = (block * block_rows + row) * channels + channel;
                require(decoded[row] == values[static_cast<std::size_t>(index)], "lossless signed roundtrip");
                require(raw.value(block * block_rows + row, channel) == decoded[row], "raw scalar read");
            }
            const auto planes = packed.plane_view(block, channel, 0, 32);
            require(packed.read_planes(block, channel, 0, 32, copied_planes) == planes.size(), "plane copy byte count");
            require(std::equal(planes.begin(), planes.end(), copied_planes.begin()), "plane copy");
            // Reconstruct coarse prefixes without using decode_block, and establish
            // that every exact value lies in every progressive interval.
            for (std::uint32_t bits : {1U, 2U, 4U, 8U, 16U, 31U, 32U}) {
                const auto prefix_planes = packed.plane_view(block, channel, 0, bits);
                const auto stride = packed.plane_bytes(block);
                for (std::uint32_t row = 0; row < rows_here; ++row) {
                    std::uint32_t prefix = 0;
                    for (std::uint32_t plane = 0; plane < bits; ++plane) {
                        const auto bit = (static_cast<std::uint32_t>(prefix_planes[plane * stride + row / 8]) >> (row % 8)) & 1U;
                        prefix |= bit << (31 - plane);
                    }
                    const auto lower = static_cast<std::int64_t>(prefix) - 2147483648LL;
                    const auto width = bits == 32 ? 0ULL : ((1ULL << (32 - bits)) - 1);
                    const auto upper = lower + static_cast<std::int64_t>(width);
                    require(lower <= decoded[row] && decoded[row] <= upper, "progressive interval contains exact value");
                }
            }
        }
    }
    must_throw([&] { (void)raw.value(rows, 0); }, "raw row bounds");
    must_throw([&] { (void)raw.value(0, channels); }, "raw channel bounds");
    must_throw([&] { (void)packed.block_length(packed.block_count()); }, "packed block bounds");
    if (rows) {
        must_throw([&] { (void)packed.plane_view(0, channels, 0, 1); }, "packed channel bounds");
        must_throw([&] { (void)packed.plane_view(0, 0, 31, 2); }, "packed bit bounds");
        must_throw([&] { (void)packed.plane_view(0, 0, UINT32_MAX, 1); }, "packed first-bit overflow");
        must_throw([&] { packed.decode_block(0, 0, {}); }, "decode output bounds");
    }
    const auto before = read_bytes(packed_path);
    must_throw([&] { (void)suffice::pack_file(raw_path, packed_path, channels, block_rows); }, "overwrite refused");
    require(before == read_bytes(packed_path), "existing destination preserved");
    must_throw([&] { (void)suffice::pack_file(raw_path, raw_path, channels, block_rows); }, "same input and output refused");
    require(read_bytes(raw_path).size() == values.size() * 4, "source preserved");
}

void check_corruption() {
    TempDirectory directory;
    const auto raw_path = directory.path / "input.i32";
    const auto good_path = directory.path / "good.sfc";
    const auto bad_path = directory.path / "bad.sfc";
    const std::array<std::int32_t, 3> values{-1, 0, 1};
    write_values(raw_path, values);
    const auto info = suffice::pack_file(raw_path, good_path, 1, 32);
    const auto original = read_bytes(good_path);
    auto rejected = [&](std::vector<std::uint8_t> bytes) {
        write_bytes(bad_path, bytes);
        must_throw([&] { suffice::PackedDataset bad(bad_path); }, "invalid header rejected");
        must_throw([&] { (void)suffice::PackedDataset::copy_bytes(bytes); }, "invalid owned packed header rejected");
    };
    for (std::size_t length : {0U, 7U, 63U, 71U}) {
        rejected(std::vector<std::uint8_t>(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length)));
    }
    auto modified = original; modified[0] ^= 1; rejected(modified);
    modified = original; modified[8] = 2; rejected(modified);
    modified = original; modified[64] ^= 1; rejected(modified);
    modified = original; put_u32(modified.data() + 24, 0); rejected(modified);
    modified = original; put_u32(modified.data() + 28, 33); rejected(modified);
    modified = original; put_u64(modified.data() + 16, UINT64_MAX); rejected(modified);
    modified = original; modified.push_back(0); rejected(modified);
    modified = original; modified.pop_back(); rejected(modified);
    // Payload corruption is not scanned by construction, but the explicit full
    // verification pass rejects it before metadata is used for exact execution.
    modified = original; modified.back() ^= 1;
    write_bytes(bad_path, modified);
    { suffice::PackedDataset bad(bad_path); must_throw([&] { bad.verify_payload(); }, "payload checksum mismatch"); }
    { auto bad = suffice::PackedDataset::copy_bytes(modified);
      modified = original;
      must_throw([&] { bad.verify_payload(); }, "owned payload corruption survived caller repair"); }
    // A checksummed but incorrect declared range is independently rejected.
    modified = original; put_u32(modified.data() + 64, std::bit_cast<std::uint32_t>(std::int32_t{-2}));
    refresh_metadata(modified, static_cast<std::size_t>(info.metadata_bytes));
    write_bytes(bad_path, modified);
    { suffice::PackedDataset bad(bad_path); must_throw([&] { bad.verify_payload(); }, "declared range mismatch"); }
    { auto bad = suffice::PackedDataset::copy_bytes(modified);
      must_throw([&] { bad.verify_payload(); }, "owned declared range mismatch"); }
    // Recomputed checksums do not legitimize noncanonical padding.
    modified = original; modified[static_cast<std::size_t>(info.metadata_bytes)] |= 0x80U;
    put_u64(modified.data() + 40, fnv(std::span<const std::uint8_t>(modified).subspan(static_cast<std::size_t>(info.metadata_bytes))));
    refresh_metadata(modified, static_cast<std::size_t>(info.metadata_bytes));
    write_bytes(bad_path, modified);
    { suffice::PackedDataset bad(bad_path); must_throw([&] { bad.verify_payload(); }, "padding mismatch"); }
    { auto bad = suffice::PackedDataset::copy_bytes(modified);
      must_throw([&] { bad.verify_payload(); }, "owned padding mismatch"); }
    const std::array<std::uint8_t, 3> partial_raw{1, 2, 3};
    write_bytes(bad_path, partial_raw);
    must_throw([&] { suffice::RawDataset bad(bad_path, 1); }, "partial raw row");
    must_throw([&] { (void)suffice::RawDataset::copy_bytes(partial_raw, 1); }, "owned partial raw row");
    must_throw([&] { suffice::RawDataset bad(raw_path, 0); }, "zero raw channels");
    must_throw([&] { (void)suffice::RawDataset::copy_bytes({}, 0); }, "owned zero raw channels");
    must_throw([&] { (void)suffice::RawDataset::copy_bytes({}, 1025); }, "owned excessive raw channels");
    const std::array<std::uint8_t, 4> one_value{};
    must_throw([&] { (void)suffice::RawDataset::copy_bytes(one_value, 2); }, "owned incomplete multichannel row");
    must_throw([&] { (void)suffice::pack_file(raw_path, directory.path / "invalid.sfc", 1, 63); }, "invalid output block size");
}

void check_owned_bytes() {
    TempDirectory directory;
    const auto raw_path = directory.path / "input.i32";
    const auto packed_path = directory.path / "input.sfc";
    constexpr std::uint32_t rows = 257, channels = 3, block_rows = 64;
    std::vector<std::int32_t> values(rows * channels);
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = std::bit_cast<std::int32_t>((static_cast<std::uint32_t>(i) + 1) * 0x9e3779b9U);
    values.front() = std::numeric_limits<std::int32_t>::min();
    values.back() = std::numeric_limits<std::int32_t>::max();
    write_values(raw_path, values);
    const auto packed_info = suffice::pack_file(raw_path, packed_path, channels, block_rows);

    // Both factories must own real bytes, not retain the caller's spans. The
    // source is overwritten and destroyed before the first dataset read.
    auto raw = [&] {
        auto source = read_bytes(raw_path);
        auto owned = suffice::RawDataset::copy_bytes(source, channels);
        require(owned.bytes().data() != source.data(), "raw factory aliases caller storage");
        std::fill(source.begin(), source.end(), std::uint8_t{0});
        return owned;
    }();
    require(raw.info().rows == rows && raw.info().channels == channels &&
            raw.info().file_bytes == values.size() * 4, "owned raw dimensions");
    std::vector<std::int32_t> observed(values.size());
    raw.read_rows(0, rows, observed);
    require(observed == values, "owned raw changed after caller overwrite/destruction");

    const auto raw_view = raw.bytes();
    suffice::RawDataset moved_raw(std::move(raw));
    require(moved_raw.bytes().data() == raw_view.data() && moved_raw.bytes().size() == raw_view.size(),
            "raw move construction relocated owned bytes");
    // Reuse the moved-from object without reading its unspecified old state.
    raw = suffice::RawDataset::copy_bytes({}, 1);
    require(raw.info().rows == 0 && raw.bytes().empty(), "moved-from raw reassignment");
    suffice::RawDataset assigned_raw(raw_path, channels);
    assigned_raw = std::move(moved_raw); // Release a mapping, acquire an owned copy.
    require(assigned_raw.bytes().data() == raw_view.data(), "raw move assignment relocated owned bytes");
    assigned_raw.read_rows(0, rows, observed);
    require(observed == values, "owned raw move assignment lost data");
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        require(assigned_raw.value(0, channel) == values[channel], "owned raw first row");
        require(assigned_raw.value(rows - 1, channel) == values[(rows - 1) * channels + channel], "owned raw final row");
    }
    must_throw([&] { assigned_raw.read_rows(rows, 1, observed); }, "owned raw range bounds");
    must_throw([&] { assigned_raw.read_rows(0, 1, {}); }, "owned raw output bounds");

    auto packed = [&] {
        auto source = read_bytes(packed_path);
        auto owned = suffice::PackedDataset::copy_bytes(source);
        require(owned.plane_view(0, 0, 0, 32).data() != source.data() + packed_info.metadata_bytes,
                "packed factory aliases caller storage");
        std::fill(source.begin(), source.end(), std::uint8_t{0});
        return owned;
    }();
    require(packed.info().rows == rows && packed.info().channels == channels &&
            packed.info().block_rows == block_rows && packed.info().file_bytes == packed_info.file_bytes,
            "owned packed dimensions");
    packed.verify_payload();
    std::vector<std::int32_t> decoded(block_rows);
    const auto check_packed = [&](const suffice::PackedDataset& data) {
        data.verify_payload();
        require(data.block_count() == 5 && data.block_length(4) == 1, "owned packed tail geometry");
        for (std::uint64_t block = 0; block < data.block_count(); ++block) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                data.decode_block(block, channel, decoded);
                for (std::uint32_t row = 0; row < data.block_length(block); ++row)
                    require(decoded[row] == values[(block * block_rows + row) * channels + channel],
                            "owned packed changed after caller overwrite/destruction or move");
            }
        }
    };
    check_packed(packed);
    const auto packed_view = packed.plane_view(0, 0, 0, 32);
    const auto first_plane_bytes = std::vector<std::uint8_t>(packed_view.begin(), packed_view.end());
    suffice::PackedDataset moved_packed(std::move(packed));
    require(moved_packed.plane_view(0, 0, 0, 32).data() == packed_view.data(),
            "packed move construction relocated owned bytes");
    suffice::PackedDataset assigned_packed(packed_path);
    assigned_packed = std::move(moved_packed); // Release a mapping, acquire an owned copy.
    require(assigned_packed.plane_view(0, 0, 0, 32).data() == packed_view.data(),
            "packed move assignment relocated owned bytes");
    require(std::equal(packed_view.begin(), packed_view.end(), first_plane_bytes.begin()),
            "borrowed packed view changed while backing owner moved");
    check_packed(assigned_packed);
    packed = suffice::PackedDataset(packed_path);
    packed.verify_payload();
    must_throw([&] { (void)assigned_packed.plane_view(5, 0, 0, 32); }, "owned packed block bounds");
    must_throw([&] { (void)assigned_packed.plane_view(0, channels, 0, 1); }, "owned packed channel bounds");
    must_throw([&] { (void)assigned_packed.plane_view(0, 0, 32, 1); }, "owned packed bit bounds");

    // Empty raw input is legal, but an empty encoded byte span lacks a header.
    auto empty_raw = suffice::RawDataset::copy_bytes({}, 1024);
    require(empty_raw.info().rows == 0 && empty_raw.info().channels == 1024 && empty_raw.bytes().empty(),
            "empty owned raw input");
    empty_raw.read_rows(0, 0, {});
    must_throw([&] { (void)suffice::PackedDataset::copy_bytes({}); }, "empty owned packed header rejected");
    const auto empty_path = directory.path / "empty.i32";
    const auto empty_packed_path = directory.path / "empty.sfc";
    write_bytes(empty_path, {});
    (void)suffice::pack_file(empty_path, empty_packed_path, channels, block_rows);
    const auto empty_encoded = read_bytes(empty_packed_path);
    auto empty_packed = suffice::PackedDataset::copy_bytes(empty_encoded);
    empty_packed.verify_payload();
    require(empty_packed.info().rows == 0 && empty_packed.info().channels == channels &&
            empty_packed.block_count() == 0, "empty owned packed dimensions");
    empty_packed = std::move(assigned_packed); // Release an owned vector, acquire another.
    check_packed(empty_packed);
}
} // namespace

void test_storage() {
    for (auto rows : {0U, 1U, 7U, 8U, 31U, 32U, 33U, 63U, 64U, 65U, 255U, 257U}) {
        check_roundtrip(rows, 3, 64);
    }
    check_roundtrip(97, 1, 32);
    check_roundtrip(1027, 4, 256);
    check_corruption();
    check_owned_bytes();
}
