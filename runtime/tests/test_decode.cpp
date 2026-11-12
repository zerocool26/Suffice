#include "suffice/storage.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check_decode(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("decode: ") + message);
}

struct DecodeDirectory {
    std::filesystem::path path;
    DecodeDirectory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-decode-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create decode test directory");
    }
    ~DecodeDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void write_decode_raw(const std::filesystem::path& path, std::span<const std::int32_t> values) {
    std::ofstream output(path, std::ios::binary);
    for (auto value : values) {
        const auto word = std::bit_cast<std::uint32_t>(value);
        for (unsigned b = 0; b < 4; ++b) output.put(static_cast<char>((word >> (8 * b)) & 255U));
    }
    output.close();
    if (!output) throw std::runtime_error("cannot write decode fixture");
}

std::vector<std::int32_t> decode_values(std::size_t rows, std::uint32_t channels) {
    std::vector<std::int32_t> values(rows * channels);
    std::uint32_t state = 0xc07f3d59U ^ static_cast<std::uint32_t>(rows);
    for (std::size_t i = 0; i < values.size(); ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        switch (i % 17) {
        case 0: values[i] = std::numeric_limits<std::int32_t>::min(); break;
        case 1: values[i] = std::numeric_limits<std::int32_t>::max(); break;
        case 2: values[i] = -1; break;
        case 3: values[i] = 0; break;
        case 4: values[i] = 1; break;
        case 5: values[i] = std::bit_cast<std::int32_t>(0x55555555U); break;
        case 6: values[i] = std::bit_cast<std::int32_t>(0xaaaaaaaaU); break;
        default: values[i] = std::bit_cast<std::int32_t>(state); break;
        }
    }
    return values;
}

// Deliberately scalar reference, independent of the butterfly implementation.
void scalar_decode(const suffice::PackedDataset& data, std::uint64_t block,
                   std::uint32_t channel, std::span<std::int32_t> output) {
    const auto rows = data.block_length(block);
    const auto planes = data.plane_view(block, channel, 0, 32);
    const auto stride = data.plane_bytes(block);
    for (std::size_t row = 0; row < rows; ++row) {
        std::uint32_t value = 0;
        for (std::size_t p = 0; p < 32; ++p) {
            value |= ((std::uint32_t(planes[p * stride + row / 8]) >> (row % 8)) & 1U) << (31 - p);
        }
        output[row] = std::bit_cast<std::int32_t>(value ^ 0x80000000U);
    }
}

void verify_decode_fixture(std::span<const std::int32_t> values, std::uint32_t channels,
                           std::uint32_t block_rows) {
    DecodeDirectory directory;
    const auto raw = directory.path / "input.i32";
    const auto packed = directory.path / "input.sfp";
    write_decode_raw(raw, values);
    (void)suffice::pack_file(raw, packed, channels, block_rows);
    suffice::PackedDataset data(packed);
    data.verify_payload();
    constexpr std::int32_t sentinel = 0x13572468;
    std::vector<std::int32_t> actual(block_rows + 1, sentinel), reference(block_rows + 1, sentinel);
    for (std::uint64_t block = 0; block < data.block_count(); ++block) {
        const auto length = data.block_length(block);
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            std::fill(actual.begin(), actual.end(), sentinel);
            std::fill(reference.begin(), reference.end(), sentinel);
            scalar_decode(data, block, channel, reference);
            allocation_probe::start();
            try { data.decode_block(block, channel, actual); }
            catch (...) { (void)allocation_probe::stop(); throw; }
            const auto allocations = allocation_probe::stop();
            check_decode(allocations == 0, "decode allocated heap memory");
            check_decode(actual == reference, "transpose differs from scalar reference or writes past length");
            for (std::uint32_t row = 0; row < length; ++row) {
                const auto expected = values[(block * block_rows + row) * channels + channel];
                check_decode(actual[row] == expected, "decoded value differs from original raw value");
            }
        }
    }
}

void all_decode_block_sizes() {
    for (std::uint32_t block = 32; block <= 65536; block *= 2) {
        std::vector<std::size_t> lengths{0,1,2,3,7,8,9,15,16,17,23,24,25,31,32,33,63,64,65,
                                       block - 1, block, block + 1, 2 * block + 13};
        std::sort(lengths.begin(), lengths.end());
        lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
        for (const auto channels : {1U, 3U}) {
            for (const auto rows : lengths) verify_decode_fixture(decode_values(rows, channels), channels, block);
        }
    }
}

void bit_matrix_basis() {
    // Every input bit of a 32x32 tile is exercised in isolation. The ordinary
    // numeric values corresponding to a zero biased tile are INT32_MIN.
    std::vector<std::int32_t> values(32 * 32 * 32, std::numeric_limits<std::int32_t>::min());
    for (unsigned plane = 0; plane < 32; ++plane) {
        for (unsigned row = 0; row < 32; ++row) {
            values[(plane * 32 + row) * 32 + row] =
                std::bit_cast<std::int32_t>((std::uint32_t{1} << (31 - plane)) ^ 0x80000000U);
        }
    }
    verify_decode_fixture(values, 1, 32);
    verify_decode_fixture(values, 1, 65536);
}
} // namespace

void test_decode() {
    all_decode_block_sizes();
    bit_matrix_basis();
}

#ifdef SUFFICE_DECODE_STANDALONE
namespace {
std::atomic<bool> decode_track{false};
std::atomic<std::uint64_t> decode_allocations{0};
void* decode_allocate(std::size_t size) {
    if (decode_track.load(std::memory_order_relaxed)) decode_allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t size) { return decode_allocate(size); }
void* operator new[](std::size_t size) { return decode_allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace allocation_probe {
void start() noexcept { decode_allocations.store(0); decode_track.store(true); }
std::uint64_t stop() noexcept { decode_track.store(false); return decode_allocations.load(); }
}

int main() {
    try {
        test_decode();
        std::cout << "PASS transpose basis, all block sizes, full int32 values, tails, channels, and allocation discipline\n";
        DecodeDirectory directory;
        const auto raw = directory.path / "bench.i32", packed = directory.path / "bench.sfp";
        constexpr std::size_t rows = 1048576;
        const auto expected = decode_values(rows, 1);
        write_decode_raw(raw, expected);
        (void)suffice::pack_file(raw, packed, 1, 4096);
        suffice::PackedDataset data(packed);
        data.verify_payload();
        std::vector<std::int32_t> output(rows);
        auto execute = [&](bool scalar) {
            for (std::uint64_t block = 0; block < data.block_count(); ++block) {
                auto target = std::span<std::int32_t>(output).subspan(block * data.info().block_rows, data.block_length(block));
                if (scalar) scalar_decode(data, block, 0, target);
                else data.decode_block(block, 0, target);
            }
        };
        execute(true); check_decode(output == expected, "scalar warm output mismatch");
        execute(false); check_decode(output == expected, "transpose warm output mismatch");
        std::array<std::array<double,9>,2> samples{};
        for (unsigned repeat = 0; repeat < 9; ++repeat) {
            for (unsigned slot = 0; slot < 2; ++slot) {
                const auto mode = (repeat + slot) % 2;
                const auto start = std::chrono::steady_clock::now();
                execute(mode == 0);
                samples[mode][repeat] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
                check_decode(output == expected, "timed output mismatch");
            }
        }
        for (auto& sample : samples) std::sort(sample.begin(), sample.end());
        std::cout << "{\"rows\":" << rows << ",\"block_rows\":4096,\"repeat\":9,\"scalar_median_ms\":"
                  << samples[0][4] << ",\"transpose_median_ms\":" << samples[1][4]
                  << ",\"speedup\":" << samples[0][4]/samples[1][4]
                  << ",\"tile_bytes\":128,\"exact\":true}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#endif
