#include "suffice/encoded_ranges.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using suffice::Bounds;
using suffice::i64;
constexpr i64 minimum = std::numeric_limits<std::int32_t>::min();
constexpr i64 maximum = std::numeric_limits<std::int32_t>::max();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("encoded range test: ") + message);
}
template<class F> void must_throw(F&& f, const char* message) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-encoded-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create encoded test directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct Fixture {
    Directory directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::PackedDataset> packed;
    std::uint32_t channels;
    Fixture(std::vector<std::int32_t> input, std::uint32_t channel_count,
            std::uint32_t block_rows)
        : values(std::move(input)), channels(channel_count) {
        const auto raw_path = directory.path / "data.i32";
        const auto packed_path = directory.path / "data.sfc";
        std::ofstream output(raw_path, std::ios::binary);
        for (auto value : values) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned b = 0; b < 4; ++b) output.put(static_cast<char>((bits >> (8 * b)) & 255U));
        }
        output.close();
        if (!output) throw std::runtime_error("cannot write encoded test data");
        (void)suffice::pack_file(raw_path, packed_path, channels, block_rows);
        packed = std::make_unique<suffice::PackedDataset>(packed_path);
        packed->verify_payload();
    }
};

std::vector<std::int32_t> data_values(std::uint32_t rows, std::uint32_t channels) {
    std::vector<std::int32_t> values(static_cast<std::size_t>(rows) * channels);
    std::uint32_t random = 0x1284ab91U + rows;
    for (std::size_t i = 0; i < values.size(); ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        switch (i % 9) {
        case 0: values[i] = std::numeric_limits<std::int32_t>::min(); break;
        case 1: values[i] = std::numeric_limits<std::int32_t>::max(); break;
        case 2: values[i] = -1; break;
        case 3: values[i] = 0; break;
        case 4: values[i] = 1; break;
        default: values[i] = std::bit_cast<std::int32_t>(random); break;
        }
    }
    return values;
}

void check(Fixture& fixture, std::uint32_t channel, std::span<const Bounds> ranges,
           unsigned first_stage = 4) {
    const auto rows = fixture.packed->info().rows;
    std::vector<std::uint64_t> expected(suffice::mask_words(rows), 0);
    std::uint64_t expected_count = 0;
    for (std::uint64_t row = 0; row < rows; ++row) {
        const i64 value = fixture.values[static_cast<std::size_t>(row) * fixture.channels + channel];
        bool accepted = false;
        for (auto range : ranges) accepted |= range.lo <= value && value <= range.hi;
        if (accepted) { expected[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64); ++expected_count; }
    }
    std::vector<std::uint64_t> actual(expected.size(), ~std::uint64_t{0});
    suffice::EncodedRangeExecutor executor(*fixture.packed, channel, ranges, first_stage);
    allocation_probe::start();
    const auto stats = executor.run(actual);
    const auto allocations = allocation_probe::stop();
    require(allocations == 0, "prepared run allocates");
    require(actual == expected, "independent exact mask mismatch");
    require(stats.accepted == expected_count, "accepted count mismatch");
    require(stats.rows == rows, "row count mismatch");
    require(stats.output_bytes == actual.size() * sizeof(std::uint64_t), "output bytes mismatch");
    require(stats.kernel == "bitplane-preimage" || stats.kernel == "bitplane-preimage-projection",
            "kernel name mismatch");
    const auto resolved = std::accumulate(stats.resolved_by_bits.begin(), stats.resolved_by_bits.end(), std::uint64_t{0});
    require(resolved == rows, "resolution histogram does not sum to rows");
    require(stats.refined_rows <= rows, "refinement count exceeds rows");
    require(stats.loaded_planes <= 32 * fixture.packed->block_count(), "too many planes read");
    const auto payload = fixture.packed->info().file_bytes - fixture.packed->info().metadata_bytes;
    require(stats.requested_payload_bytes <= payload / fixture.channels, "read more than channel payload");
    require(stats.peak_scratch_bytes == executor.scratch_bytes(), "scratch accounting mismatch");
    const auto repeat = executor.run(actual);
    require(actual == expected && repeat.accepted == expected_count, "repeat run state was not reset");
}

void broad_cases() {
    const std::array<std::vector<Bounds>, 10> range_sets{{
        {}, {{minimum,maximum}}, {{minimum,-1}}, {{0,maximum}},
        {{-1,1}}, {{minimum,minimum},{maximum,maximum}},
        {{minimum,-1000000},{-100,100},{1000000,maximum}},
        {{-2147483600LL,-2147483500LL},{2147483500LL,2147483600LL}},
        {{-8,-8},{-6,-6},{-4,-4},{-2,-2},{0,0},{2,2},{4,4},{6,6}},
        {{-1,-1},{0,0},{1,1}}
    }};
    for (auto block : {32U,64U,128U}) {
        for (auto channels : {1U,3U}) {
            for (auto rows : {0U,1U,2U,3U,7U,31U,32U,33U,63U,64U,65U,127U,129U,257U}) {
                Fixture fixture(data_values(rows, channels), channels, block);
                for (std::uint32_t channel = 0; channel < channels; ++channel) {
                    for (const auto& ranges : range_sets) {
                        check(fixture, channel, ranges, 1);
                        check(fixture, channel, ranges, 4);
                        check(fixture, channel, ranges, 32);
                    }
                }
            }
        }
    }
}

void exhaustive_endpoints() {
    std::vector<std::int32_t> values;
    values.push_back(std::numeric_limits<std::int32_t>::min());
    for (int i = -40; i <= 40; ++i) values.push_back(i);
    values.push_back(std::numeric_limits<std::int32_t>::max());
    Fixture fixture(std::move(values), 1, 64);
    for (i64 lo = -20; lo <= 20; ++lo) {
        for (i64 hi = lo; hi <= 20; ++hi) {
            const std::array<Bounds,1> ranges{{{lo,hi}}};
            check(fixture,0,ranges);
        }
    }
    // Every pair of small disjoint closed intervals, including adjacent ones.
    for (i64 a = -5; a <= 5; ++a) for (i64 b = a; b <= 5; ++b)
        for (i64 c = b + 1; c <= 5; ++c) for (i64 d = c; d <= 5; ++d) {
            const std::array<Bounds,2> ranges{{{a,b},{c,d}}};
            check(fixture,0,ranges);
        }
    std::uint32_t random = 0xe91673a1U;
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        const auto count = 1 + iteration % 8;
        std::vector<std::uint32_t> endpoints;
        while (endpoints.size() < count * 2) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            if (std::find(endpoints.begin(), endpoints.end(), random) == endpoints.end()) endpoints.push_back(random);
        }
        std::sort(endpoints.begin(), endpoints.end());
        std::vector<Bounds> ranges;
        for (unsigned i = 0; i < count; ++i) ranges.push_back({i64(endpoints[i*2])-2147483648LL,i64(endpoints[i*2+1])-2147483648LL});
        check(fixture,0,ranges);
    }
}

void clipping_and_validation() {
    Fixture fixture({100,101,150,199,200},1,32);
    for (const auto& ranges : std::vector<std::vector<Bounds>>{
             {},{{100,200}},{{minimum,99}},{{201,maximum}},{{minimum,100}},
             {{200,maximum}},{{minimum,150}},{{150,maximum}},{{100,100},{200,200}},
             {{-2,-1},{100,101},{102,200},{201,300}}}) {
        check(fixture,0,ranges);
    }
    const std::array<Bounds,1> all{{{100,200}}};
    suffice::EncodedRangeExecutor constant(*fixture.packed,0,all);
    std::vector<std::uint64_t> output(suffice::mask_words(5));
    const auto stats=constant.run(output);
    require(stats.requested_payload_bytes==0 && stats.loaded_planes==0,"domain-covering acceptance should read no planes");
    std::array<Bounds,2> overlap{{{0,1},{1,2}}};
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,overlap);},"overlapping ranges accepted");
    std::array<Bounds,2> unsorted{{{5,6},{0,1}}};
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,unsorted);},"unsorted ranges accepted");
    std::array<Bounds,1> reversed{{{2,1}}};
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,reversed);},"empty interval accepted");
    std::array<Bounds,1> outside{{{minimum-1,maximum}}};
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,outside);},"out-of-int32 interval accepted");
    std::array<Bounds,9> excess{};for(std::size_t i=0;i<excess.size();++i)excess[i]={static_cast<i64>(i*2),static_cast<i64>(i*2)};
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,excess);},"excess ranges accepted");
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,1,all);},"invalid channel accepted");
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,all,0);},"zero first stage accepted");
    must_throw([&]{suffice::EncodedRangeExecutor e(*fixture.packed,0,all,33);},"oversized first stage accepted");
    must_throw([&]{constant.run({});},"wrong output span accepted");

    // Sign classification is certified by exactly one significance plane in every
    // nonconstant block; metadata contains both signs and cannot shortcut it.
    Fixture signs({-1,0,1,-2,2,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()},1,32);
    const std::array<Bounds,1> positives{{{0,maximum}}};
    suffice::EncodedRangeExecutor sign_executor(*signs.packed,0,positives,1);
    std::vector<std::uint64_t> sign_mask(suffice::mask_words(7));
    const auto sign_stats=sign_executor.run(sign_mask);
    require(sign_stats.loaded_planes==1 && sign_stats.resolved_by_bits[1]==7 && sign_stats.refined_rows==0,"one-plane certificate failed");
}

void sign_projection_cases() {
    const std::array<std::array<Bounds,1>,2> ranges{{
        {{{0,maximum}}}, {{{minimum,-1}}}
    }};
    for (const auto block : {32U,64U,4096U}) {
        for (const auto rows : {31U,32U,33U,63U,64U,65U,129U,4097U}) {
            auto values = data_values(rows,3);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                values[channel] = static_cast<std::int32_t>(minimum);
                values[3 + channel] = static_cast<std::int32_t>(maximum);
            }
            Fixture fixture(std::move(values),3,block);
            for (const auto& accepted : ranges) {
                check(fixture,2,accepted);
                std::vector<std::uint64_t> mask(suffice::mask_words(rows),~std::uint64_t{0});
                allocation_probe::start();
                suffice::EncodedRangeExecutor executor(*fixture.packed,2,accepted);
                const auto construction_allocations = allocation_probe::stop();
                const auto stats = executor.run(mask);
                if constexpr (std::endian::native == std::endian::little) {
                    require(construction_allocations == 0,"sign projection allocated comparator buffers");
                    require(stats.kernel == "bitplane-preimage-projection","sign projection not selected");
                    require(executor.scratch_bytes() == sizeof(executor),"sign projection owns hidden scratch");
                }
                require(stats.requested_payload_bytes == (std::uint64_t{rows}+7)/8,
                        "sign projection did not request exactly one bit per row plus padding");
                require(stats.loaded_planes == fixture.packed->block_count(),
                        "sign projection did not request one plane per block");
                require(stats.resolved_by_bits[1] == rows && stats.refined_rows == 0,
                        "sign projection resolution accounting is incorrect");
                if (rows % 64) require((mask.back() >> (rows % 64)) == 0,
                                      "sign projection retained padding bits");
            }
        }
    }
}
} // namespace

void test_encoded_ranges() {
    broad_cases();
    exhaustive_endpoints();
    clipping_and_validation();
    sign_projection_cases();
}
