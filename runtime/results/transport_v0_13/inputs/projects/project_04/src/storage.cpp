#include "suffice/storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace suffice {
namespace {

constexpr std::array<std::uint8_t, 8> magic{'S','U','F','F','I','C','E','1'};
constexpr std::uint32_t version = 1;
constexpr std::size_t fixed_header = 64;
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("storage: " + message);
}

[[noreturn]] void system_fail(const std::string& operation) {
#ifdef _WIN32
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
#else
    throw std::system_error(errno, std::generic_category(), operation);
#endif
}

std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) fail("size addition overflow");
    return a + b;
}

std::uint64_t checked_mul(std::uint64_t a, std::uint64_t b) {
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a) fail("size multiplication overflow");
    return a * b;
}

void validate_channels(std::uint32_t channels) {
    if (channels == 0 || channels > 1024) fail("channels must be in [1, 1024]");
}

void validate_block_rows(std::uint32_t rows) {
    if (rows < 32 || rows > 65536 || !std::has_single_bit(rows)) {
        fail("block_rows must be a power of two in [32, 65536]");
    }
}

DatasetInfo raw_dataset_info(std::size_t length, std::uint32_t channels) {
    validate_channels(channels);
    const auto row_bytes = static_cast<std::uint64_t>(channels) * 4;
    if (length % row_bytes != 0) fail("raw file length is not a whole number of interleaved rows");
    DatasetInfo info;
    info.rows = length / row_bytes;
    info.channels = channels;
    // For owned copies this records the backing byte extent, not a disk file.
    info.file_bytes = length;
    return info;
}

std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t* data) noexcept {
    return read_u32(data) | (static_cast<std::uint64_t>(read_u32(data + 4)) << 32);
}

std::int32_t signed_bits(std::uint32_t bits) noexcept {
    return std::bit_cast<std::int32_t>(bits);
}

template<unsigned Shift, std::uint32_t Mask>
void transpose_stage(std::array<std::uint32_t, 32>& tile) noexcept {
    // Exchange one half of each pair of bit fields. Fixed shifts and masks let
    // the native compiler vectorize this portable butterfly network.
    for (unsigned base = 0; base < 32; base += 2 * Shift) {
        for (unsigned i = 0; i < Shift; ++i) {
            auto& low = tile[base + i];
            auto& high = tile[base + i + Shift];
            const auto exchange = (low ^ (high >> Shift)) & Mask;
            low ^= exchange;
            high ^= exchange << Shift;
        }
    }
}

void transpose_planes(std::array<std::uint32_t, 32>& tile) noexcept {
    transpose_stage<16, 0x0000ffffU>(tile);
    transpose_stage<8, 0x00ff00ffU>(tile);
    transpose_stage<4, 0x0f0f0f0fU>(tile);
    transpose_stage<2, 0x33333333U>(tile);
    transpose_stage<1, 0x55555555U>(tile);
}

void write_u32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) data[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void write_u64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (unsigned i = 0; i < 8; ++i) data[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint64_t hash_bytes(std::span<const std::uint8_t> bytes,
                         std::uint64_t hash = fnv_offset) noexcept {
    for (auto byte : bytes) hash = (hash ^ byte) * fnv_prime;
    return hash;
}

std::uint64_t hash_metadata(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = fnv_offset;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = (i >= 48 && i < 56) ? std::uint8_t{0} : bytes[i];
        hash = (hash ^ value) * fnv_prime;
    }
    return hash;
}

std::uint64_t expected_payload(std::uint64_t rows, std::uint32_t channels,
                               std::uint32_t block_rows) {
    const auto full = rows / block_rows;
    const auto tail = rows % block_rows;
    const auto plane_bytes = checked_add(checked_mul(full, block_rows / 8), (tail + 7) / 8);
    return checked_mul(checked_mul(plane_bytes, channels), 32);
}

// The temporary is created exclusively. Commit never overwrites a destination,
// including one created by another writer after construction.
class AtomicOutput {
public:
    explicit AtomicOutput(const std::filesystem::path& destination) : destination_(destination) {
        if (std::filesystem::exists(destination_)) fail("destination already exists: " + destination_.string());
        static std::atomic<std::uint64_t> sequence{0};
        const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            temporary_ = destination_;
            temporary_ += ".tmp-" + std::to_string(clock) + "-" + std::to_string(sequence.fetch_add(1));
#ifdef _WIN32
            handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
                system_fail("create packed temporary");
            }
#else
            fd_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd_ >= 0) return;
            if (errno != EEXIST) system_fail("create packed temporary");
#endif
        }
        fail("could not create a unique packed temporary");
    }

    ~AtomicOutput() {
        close_noexcept();
        if (!temporary_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(temporary_, ignored);
        }
    }

    void write(std::span<const std::uint8_t> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
#ifdef _WIN32
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 0x7fffffffU));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data() + done, chunk, &written, nullptr)) system_fail("write packed file");
            if (written == 0) fail("zero-length write");
            done += written;
#else
            const auto chunk = std::min<std::size_t>(bytes.size() - done, 0x7fffffffU);
            const auto written = ::write(fd_, bytes.data() + done, chunk);
            if (written < 0) {
                if (errno == EINTR) continue;
                system_fail("write packed file");
            }
            if (written == 0) fail("zero-length write");
            done += static_cast<std::size_t>(written);
#endif
        }
    }

    void rewind() {
#ifdef _WIN32
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN)) system_fail("seek packed header");
#else
        if (::lseek(fd_, 0, SEEK_SET) < 0) system_fail("seek packed header");
#endif
    }

    void commit() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) system_fail("flush packed file");
        if (!CloseHandle(handle_)) { handle_ = INVALID_HANDLE_VALUE; system_fail("close packed file"); }
        handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            system_fail("install packed file without replacement");
        }
#else
        if (::fsync(fd_) != 0) system_fail("flush packed file");
        const auto fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) system_fail("close packed file");
        if (::link(temporary_.c_str(), destination_.c_str()) != 0) {
            system_fail("install packed file without replacement");
        }
        // Once link succeeded, the complete destination is installed. Cleanup of
        // the temporary is best effort, and is also retried by the destructor.
        if (::unlink(temporary_.c_str()) != 0) return;
#endif
        temporary_.clear();
    }

private:
    void close_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }
    std::filesystem::path destination_;
    std::filesystem::path temporary_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
};

} // namespace

class MappedFile {
public:
    // MappedFile itself is never moved: dataset moves transfer its unique_ptr.
    // The const vector has no mutable interface and never changes size, keeping
    // every borrowed plane/raw address stable for this owner's lifetime.
    explicit MappedFile(std::span<const std::uint8_t> bytes)
        : owned_bytes_(bytes.begin(), bytes.end()), data_(owned_bytes_.data()),
          size_(owned_bytes_.size()), copied_(true) {}

    explicit MappedFile(const std::filesystem::path& path) {
        try {
#ifdef _WIN32
            file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ == INVALID_HANDLE_VALUE) system_fail("open input file");
            LARGE_INTEGER length{};
            if (!GetFileSizeEx(file_, &length)) system_fail("query input length");
            if (length.QuadPart < 0 || static_cast<std::uint64_t>(length.QuadPart) > std::numeric_limits<std::size_t>::max()) {
                fail("input does not fit the process address space");
            }
            size_ = static_cast<std::size_t>(length.QuadPart);
            if (size_ == 0) return;
            mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!mapping_) system_fail("create read-only file mapping");
            data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
            if (!data_) system_fail("map input file");
#else
            fd_ = ::open(path.c_str(), O_RDONLY);
            if (fd_ < 0) system_fail("open input file");
            struct stat info{};
            if (::fstat(fd_, &info) != 0) system_fail("query input length");
            if (!S_ISREG(info.st_mode)) fail("input must be a regular file");
            if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) {
                fail("input does not fit the process address space");
            }
            size_ = static_cast<std::size_t>(info.st_size);
            if (size_ == 0) return;
            void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (mapped == MAP_FAILED) system_fail("map input file");
            data_ = static_cast<const std::uint8_t*>(mapped);
#endif
        } catch (...) {
            release();
            throw;
        }
    }

    ~MappedFile() { release(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }

private:
    void release() noexcept {
        // Owned bytes are released by the vector destructor. Never pass heap
        // storage to the OS mapping APIs, including for an empty owned input.
        if (copied_) { data_ = nullptr; size_ = 0; return; }
#ifdef _WIN32
        if (data_) { UnmapViewOfFile(data_); data_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
#else
        if (data_) { ::munmap(const_cast<std::uint8_t*>(data_), size_); data_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }
    const std::vector<std::uint8_t> owned_bytes_;
    const std::uint8_t* data_{};
    std::size_t size_{};
    bool copied_{};
#ifdef _WIN32
    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{};
#else
    int fd_{-1};
#endif
};

RawDataset::RawDataset(const std::filesystem::path& path, std::uint32_t channels) {
    validate_channels(channels);
    file_ = std::make_unique<MappedFile>(path);
    info_ = raw_dataset_info(file_->bytes().size(), channels);
}

RawDataset::RawDataset(std::unique_ptr<MappedFile> file, DatasetInfo info) noexcept
    : file_(std::move(file)), info_(std::move(info)) {}

RawDataset RawDataset::copy_bytes(std::span<const std::uint8_t> bytes,
                                  std::uint32_t channels) {
    // Reject malformed dimensions before allocating a full input copy.
    auto info = raw_dataset_info(bytes.size(), channels);
    return RawDataset(std::make_unique<MappedFile>(bytes), std::move(info));
}

RawDataset::~RawDataset() = default;
RawDataset::RawDataset(RawDataset&&) noexcept = default;
RawDataset& RawDataset::operator=(RawDataset&&) noexcept = default;

std::span<const std::uint8_t> RawDataset::bytes() const noexcept { return file_->bytes(); }

std::int32_t RawDataset::value(std::uint64_t row, std::uint32_t channel) const {
    if (row >= info_.rows || channel >= info_.channels) throw std::out_of_range("raw value index");
    const auto offset = (row * info_.channels + channel) * 4;
    return signed_bits(read_u32(file_->bytes().data() + static_cast<std::size_t>(offset)));
}

void RawDataset::read_rows(std::uint64_t first, std::uint64_t count,
                           std::span<std::int32_t> output) const {
    if (first > info_.rows || count > info_.rows - first) throw std::out_of_range("raw row range");
    const auto elements = checked_mul(count, info_.channels);
    if (elements > output.size()) throw std::invalid_argument("raw output span is too small");
    const auto* data = file_->bytes().data();
    if (elements != 0) data += static_cast<std::size_t>(first * info_.channels * 4);
    for (std::size_t i = 0; i < elements; ++i) output[i] = signed_bits(read_u32(data + i * 4));
}

PackedDataset::PackedDataset(const std::filesystem::path& path)
    : PackedDataset(std::make_unique<MappedFile>(path)) {}

PackedDataset PackedDataset::copy_bytes(std::span<const std::uint8_t> bytes) {
    return PackedDataset(std::make_unique<MappedFile>(bytes));
}

PackedDataset::PackedDataset(std::unique_ptr<MappedFile> file)
    : file_(std::move(file)) {
    const auto bytes = file_->bytes();
    if (bytes.size() < fixed_header) fail("truncated packed header");
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) fail("bad packed magic");
    if (read_u32(bytes.data() + 8) != version) fail("unsupported packed version");
    info_.metadata_bytes = read_u32(bytes.data() + 12);
    info_.rows = read_u64(bytes.data() + 16);
    info_.channels = read_u32(bytes.data() + 24);
    info_.block_rows = read_u32(bytes.data() + 28);
    validate_channels(info_.channels);
    validate_block_rows(info_.block_rows);
    const auto header_bytes = fixed_header + static_cast<std::uint64_t>(info_.channels) * 8;
    if (info_.metadata_bytes != header_bytes || header_bytes > bytes.size()) fail("invalid packed metadata length");
    const auto payload_bytes = expected_payload(info_.rows, info_.channels, info_.block_rows);
    if (read_u64(bytes.data() + 32) != payload_bytes) fail("invalid packed payload length");
    if (checked_add(header_bytes, payload_bytes) != bytes.size()) fail("packed file length does not match dimensions");
    if (read_u64(bytes.data() + 56) != 0) fail("unsupported packed flags");
    if (hash_metadata(bytes.first(static_cast<std::size_t>(header_bytes))) != read_u64(bytes.data() + 48)) {
        fail("packed metadata checksum mismatch");
    }
    info_.file_bytes = bytes.size();
    payload_checksum_ = read_u64(bytes.data() + 40);
    info_.ranges.reserve(info_.channels);
    for (std::uint32_t ch = 0; ch < info_.channels; ++ch) {
        const auto* range = bytes.data() + fixed_header + static_cast<std::size_t>(ch) * 8;
        ChannelRange item{signed_bits(read_u32(range)), signed_bits(read_u32(range + 4))};
        if (item.min > item.max) fail("invalid packed channel range");
        if (info_.rows == 0 && (item.min != 0 || item.max != 0)) fail("empty input must have zero channel ranges");
        info_.ranges.push_back(item);
    }
}

PackedDataset::~PackedDataset() = default;
PackedDataset::PackedDataset(PackedDataset&&) noexcept = default;
PackedDataset& PackedDataset::operator=(PackedDataset&&) noexcept = default;

std::uint64_t PackedDataset::block_count() const noexcept {
    return info_.rows == 0 ? 0 : 1 + (info_.rows - 1) / info_.block_rows;
}

std::uint32_t PackedDataset::block_length(std::uint64_t block) const {
    if (block >= block_count()) throw std::out_of_range("packed block index");
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(info_.block_rows,
                                                info_.rows - block * info_.block_rows));
}

std::size_t PackedDataset::plane_bytes(std::uint64_t block) const {
    return (static_cast<std::size_t>(block_length(block)) + 7) / 8;
}

std::span<const std::uint8_t> PackedDataset::plane_view(
    std::uint64_t block, std::uint32_t channel,
    std::uint32_t first_bit, std::uint32_t count_bits) const {
    if (channel >= info_.channels) throw std::out_of_range("packed channel index");
    if (first_bit > 32 || count_bits > 32 - first_bit) throw std::out_of_range("packed bit range");
    const auto per_plane = plane_bytes(block);
    const auto full_stride = static_cast<std::uint64_t>(info_.channels) * 32 * (info_.block_rows / 8);
    const auto offset = info_.metadata_bytes + block * full_stride +
                        (static_cast<std::uint64_t>(channel) * 32 + first_bit) * per_plane;
    const auto length = static_cast<std::size_t>(count_bits) * per_plane;
    return file_->bytes().subspan(static_cast<std::size_t>(offset), length);
}

std::size_t PackedDataset::read_planes(std::uint64_t block, std::uint32_t channel,
                                     std::uint32_t first_bit, std::uint32_t count_bits,
                                     std::span<std::uint8_t> output) const {
    const auto source = plane_view(block, channel, first_bit, count_bits);
    if (output.size() < source.size()) throw std::invalid_argument("plane output span is too small");
    std::copy(source.begin(), source.end(), output.begin());
    return source.size();
}

void PackedDataset::decode_block(std::uint64_t block, std::uint32_t channel,
                                 std::span<std::int32_t> output) const {
    const auto rows = block_length(block);
    if (output.size() < rows) throw std::invalid_argument("decode output span is too small");
    const auto planes = plane_view(block, channel, 0, 32);
    const auto stride = plane_bytes(block);
    std::array<std::uint32_t, 32> tile;
    std::size_t row = 0;
    for (; row + 32 <= rows; row += 32) {
        for (std::size_t plane = 0; plane < 32; ++plane) {
            tile[plane] = read_u32(planes.data() + plane * stride + row / 8);
        }
        transpose_planes(tile);
        // Input plane 0 is the value's MSB, while input row 0 is a plane's LSB.
        // The butterfly maps (plane, row) to (31-row, 31-plane); reversing the
        // output word order therefore restores increasing sample order.
        for (std::size_t i = 0; i < 32; ++i) output[row + i] = signed_bits(tile[31 - i] ^ 0x80000000U);
    }
    if (row != rows) {
        const auto tail_bytes = (static_cast<std::size_t>(rows) - row + 7) / 8;
        for (std::size_t plane = 0; plane < 32; ++plane) {
            const auto* source = planes.data() + plane * stride + row / 8;
            std::uint32_t word = 0;
            for (std::size_t b = 0; b < tail_bytes; ++b) word |= std::uint32_t(source[b]) << (8 * b);
            tile[plane] = word;
        }
        transpose_planes(tile);
        for (std::size_t i = 0; row + i < rows; ++i) output[row + i] = signed_bits(tile[31 - i] ^ 0x80000000U);
    }
}

void PackedDataset::verify_payload() const {
    const auto data = file_->bytes().subspan(static_cast<std::size_t>(info_.metadata_bytes));
    if (hash_bytes(data) != payload_checksum_) fail("packed payload checksum mismatch");
    std::vector<ChannelRange> observed(info_.channels,
        {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min()});
    std::vector<std::int32_t> values(info_.block_rows);
    for (std::uint64_t block = 0; block < block_count(); ++block) {
        const auto rows = block_length(block);
        const auto stride = plane_bytes(block);
        for (std::uint32_t channel = 0; channel < info_.channels; ++channel) {
            const auto planes = plane_view(block, channel, 0, 32);
            if (rows % 8 != 0) {
                const auto padding_mask = static_cast<std::uint8_t>(0xffU << (rows % 8));
                for (std::size_t plane = 0; plane < 32; ++plane) {
                    if ((planes[(plane + 1) * stride - 1] & padding_mask) != 0) {
                        fail("nonzero unused bits in final packed block");
                    }
                }
            }
            decode_block(block, channel, values);
            for (std::uint32_t row = 0; row < rows; ++row) {
                observed[channel].min = std::min(observed[channel].min, values[row]);
                observed[channel].max = std::max(observed[channel].max, values[row]);
            }
        }
    }
    for (std::uint32_t channel = 0; channel < info_.channels; ++channel) {
        if (info_.rows == 0) observed[channel] = {0, 0};
        if (observed[channel].min != info_.ranges[channel].min ||
            observed[channel].max != info_.ranges[channel].max) {
            fail("packed channel range does not match payload");
        }
    }
}

DatasetInfo pack_file(const std::filesystem::path& raw,
                      const std::filesystem::path& destination,
                      std::uint32_t channels, std::uint32_t block_rows) {
    validate_channels(channels);
    validate_block_rows(block_rows);
    if (std::filesystem::absolute(raw).lexically_normal() ==
        std::filesystem::absolute(destination).lexically_normal()) fail("input and output paths are identical");
    RawDataset input(raw, channels);
    AtomicOutput output(destination);
    DatasetInfo info;
    info.rows = input.info().rows;
    info.channels = channels;
    info.block_rows = block_rows;
    info.metadata_bytes = fixed_header + static_cast<std::uint64_t>(channels) * 8;
    const auto payload_bytes = expected_payload(info.rows, channels, block_rows);
    info.file_bytes = checked_add(info.metadata_bytes, payload_bytes);
    info.ranges.assign(channels,
        {std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min()});
    std::vector<std::uint8_t> header(static_cast<std::size_t>(info.metadata_bytes), 0);
    output.write(header);
    std::vector<std::uint32_t> biased_values(block_rows);
    std::vector<std::uint8_t> planes(static_cast<std::size_t>(block_rows / 8) * 32);
    std::uint64_t payload_hash = fnv_offset;
    for (std::uint64_t first = 0; first < info.rows;) {
        const auto rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(block_rows, info.rows - first));
        const auto stride = (static_cast<std::size_t>(rows) + 7) / 8;
        const auto plane_size = stride * 32;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            for (std::uint32_t row = 0; row < rows; ++row) {
                const auto value = input.value(first + row, channel);
                biased_values[row] = std::bit_cast<std::uint32_t>(value) ^ 0x80000000U;
                info.ranges[channel].min = std::min(info.ranges[channel].min, value);
                info.ranges[channel].max = std::max(info.ranges[channel].max, value);
            }
            std::fill_n(planes.begin(), plane_size, std::uint8_t{0});
            for (std::size_t plane = 0; plane < 32; ++plane) {
                for (std::size_t row = 0; row < rows; ++row) {
                    const auto bit = (biased_values[row] >> (31 - plane)) & 1U;
                    planes[plane * stride + row / 8] |= static_cast<std::uint8_t>(bit << (row % 8));
                }
            }
            const auto bytes = std::span<const std::uint8_t>(planes.data(), plane_size);
            payload_hash = hash_bytes(bytes, payload_hash);
            output.write(bytes);
        }
        first += rows;
    }
    if (info.rows == 0) std::fill(info.ranges.begin(), info.ranges.end(), ChannelRange{0, 0});
    std::copy(magic.begin(), magic.end(), header.begin());
    write_u32(header.data() + 8, version);
    write_u32(header.data() + 12, static_cast<std::uint32_t>(info.metadata_bytes));
    write_u64(header.data() + 16, info.rows);
    write_u32(header.data() + 24, channels);
    write_u32(header.data() + 28, block_rows);
    write_u64(header.data() + 32, payload_bytes);
    write_u64(header.data() + 40, payload_hash);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        auto* range = header.data() + fixed_header + static_cast<std::size_t>(channel) * 8;
        write_u32(range, std::bit_cast<std::uint32_t>(info.ranges[channel].min));
        write_u32(range + 4, std::bit_cast<std::uint32_t>(info.ranges[channel].max));
    }
    write_u64(header.data() + 48, hash_metadata(header));
    output.rewind();
    output.write(header);
    output.commit();
    return info;
}

} // namespace suffice
