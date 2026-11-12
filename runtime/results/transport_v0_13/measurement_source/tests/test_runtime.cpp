#include "suffice/runtime.hpp"
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
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using suffice::Bounds;
using suffice::CompiledProgram;
using suffice::ExecutionStats;
using suffice::i64;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("runtime test: " + message);
}

template<class Function>
void must_throw(Function function, const char* message) {
    bool threw = false;
    try { function(); }
    catch (const std::exception&) { threw = true; }
    require(threw, message);
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        static std::atomic<unsigned> counter{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-runtime-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(counter.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("could not create runtime test directory");
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct Fixture {
    TempDirectory directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::RawDataset> raw;
    std::unique_ptr<suffice::PackedDataset> packed;
    std::uint32_t channels;
    std::uint64_t rows;

    Fixture(std::uint64_t row_count, std::uint32_t channel_count, std::uint32_t block_rows)
        : values(static_cast<std::size_t>(row_count) * channel_count),
          channels(channel_count), rows(row_count) {
        std::uint32_t state = 0x9e3779b9u ^ static_cast<std::uint32_t>(row_count) ^ (channel_count << 24);
        for (std::size_t i = 0; i < values.size(); ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            switch (i % 11) {
            case 0: values[i] = std::numeric_limits<std::int32_t>::min(); break;
            case 1: values[i] = std::numeric_limits<std::int32_t>::max(); break;
            case 2: values[i] = 0; break;
            case 3: values[i] = -1; break;
            case 4: values[i] = 1; break;
            case 5: values[i] = 100000; break;
            case 6: values[i] = -100000; break;
            default: values[i] = std::bit_cast<std::int32_t>(state); break;
            }
        }
        const auto raw_path = directory.path / "input.i32";
        std::ofstream output(raw_path, std::ios::binary);
        for (const auto value : values) {
            const auto word = std::bit_cast<std::uint32_t>(value);
            const std::array<char, 4> bytes{{
                static_cast<char>(word & 255), static_cast<char>((word >> 8) & 255),
                static_cast<char>((word >> 16) & 255), static_cast<char>((word >> 24) & 255)}};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        output.close();
        if (!output) throw std::runtime_error("failed to write runtime fixture");
        const auto packed_path = directory.path / "input.sfc";
        (void)suffice::pack_file(raw_path, packed_path, channels, block_rows);
        raw = std::make_unique<suffice::RawDataset>(raw_path, channels);
        packed = std::make_unique<suffice::PackedDataset>(packed_path);
        packed->verify_payload();
    }

    [[nodiscard]] std::vector<Bounds> ranges() const {
        std::vector<Bounds> result;
        for (const auto range : packed->info().ranges) result.push_back({range.min, range.max});
        return result;
    }

    [[nodiscard]] i64 sample(std::uint64_t row, std::uint32_t channel, int offset = 0) const {
        i64 index = static_cast<i64>(row) + offset;
        index = std::clamp<i64>(index, 0, static_cast<i64>(rows - 1));
        return values[static_cast<std::size_t>(index) * channels + channel];
    }
};

i64 absolute(i64 value) { return value < 0 ? -value : value; }

template<class Score>
std::vector<std::uint64_t> reference_mask(const Fixture& fixture, i64 threshold, Score score) {
    std::vector<std::uint64_t> output(suffice::mask_words(fixture.rows), 0);
    for (std::uint64_t row = 0; row < fixture.rows; ++row) {
        if (score(row) > threshold)
            output[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64);
    }
    return output;
}

void check_statistics(const ExecutionStats& stats, const std::vector<std::uint64_t>& mask,
                      std::uint64_t rows, const std::string& context) {
    require(stats.rows == rows, context + ": incorrect row accounting");
    const auto resolved = std::accumulate(stats.resolved_by_bits.begin(), stats.resolved_by_bits.end(),
                                          std::uint64_t{0});
    require(resolved == rows, context + ": resolved histogram does not sum to rows");
    std::uint64_t accepted = 0;
    for (const auto word : mask) accepted += static_cast<std::uint64_t>(std::popcount(word));
    require(stats.accepted == accepted, context + ": accepted count differs from output mask");
    require(stats.output_bytes == mask.size() * sizeof(std::uint64_t), context + ": output byte accounting");
    require(stats.refined_rows <= rows, context + ": refined rows exceed total rows");
    if (rows % 64 && !mask.empty())
        require((mask.back() >> (rows % 64)) == 0, context + ": padding output bits were not cleared");
}

void check_engines(const Fixture& fixture, const CompiledProgram& plan,
                   const std::vector<std::uint64_t>& expected, std::string_view builtin,
                   const std::string& context, bool test_generic_identity = false) {
    std::vector<std::uint64_t> actual(expected.size(), ~std::uint64_t{0});
    auto stats = suffice::run_dense(*fixture.raw, plan, actual, builtin, true);
    require(actual == expected, context + ": dense SIMD/default mask mismatch");
    check_statistics(stats, actual, fixture.rows, context + " dense");

    std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
    stats = suffice::run_dense(*fixture.raw, plan, actual, builtin, false);
    require(actual == expected, context + ": dense scalar mask mismatch");
    check_statistics(stats, actual, fixture.rows, context + " scalar");

    std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
    stats = suffice::run_dense(*fixture.raw, plan, actual, {}, false);
    require(actual == expected, context + ": dense generic IR mask mismatch");
    check_statistics(stats, actual, fixture.rows, context + " generic");

    // The dense comparison receives the same inverse-image lowering as the
    // packed path. Exercise both its vectorized and scalar implementations.
    for (const bool allow_simd : {true, false}) {
        suffice::DenseExecutor lowered(*fixture.raw, plan, builtin, allow_simd, true);
        std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
        stats = lowered.run(actual);
        require(actual == expected, context + ": prepared dense preimage mask mismatch");
        check_statistics(stats, actual, fixture.rows, context + " dense preimage");
    }

    std::vector<std::int32_t> scratch(static_cast<std::size_t>(fixture.packed->info().block_rows) *
                                       fixture.channels * 3);
    std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
    stats = suffice::run_packed_full(*fixture.packed, plan, actual, scratch);
    require(actual == expected, context + ": packed full mask mismatch");
    check_statistics(stats, actual, fixture.rows, context + " packed full");

    const std::array<std::vector<unsigned>, 3> stage_sets{{
        {1, 2, 4, 8, 16, 32}, {4, 8, 16, 32}, {32}
    }};
    for (const auto& stages : stage_sets) {
        const unsigned specializations = test_generic_identity ? 2u : 1u;
        for (unsigned mode = 0; mode < specializations; ++mode) {
            suffice::ExecutionOptions options;
            options.stages = stages;
            options.specialize = mode == 0;
            suffice::ProgressiveExecutor engine(*fixture.packed, plan, options);
            for (int replay = 0; replay < 2; ++replay) {
                std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
                stats = engine.run(actual);
                require(actual == expected, context + ": progressive mask mismatch");
                check_statistics(stats, actual, fixture.rows, context + " progressive");
                require(stats.peak_scratch_bytes == engine.scratch_bytes(), context + ": scratch accounting");
                if (stages.size() == 1 && stages[0] == 32)
                    require(stats.refined_rows == 0, context + ": exact initial stage reports refinement");
            }
        }
    }
}

void builtin_matrix() {
    const std::array<std::uint64_t, 14> lengths{{0, 1, 2, 7, 31, 32, 33, 63, 64, 65, 127, 128, 129, 257}};
    const std::array<std::uint32_t, 3> block_sizes{{32, 64, 128}};
    const std::array<i64, 8> identity_thresholds{{
        std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max(),
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(),
        -1, 0, 1, 100000
    }};
    for (const auto rows : lengths) {
        for (const auto channels : {1u, 2u}) {
            for (const auto block_rows : block_sizes) {
                Fixture fixture(rows, channels, block_rows);
                const auto ranges = fixture.ranges();
                for (std::uint32_t channel = 0; channel < channels; ++channel) {
                    const auto context = "rows=" + std::to_string(rows) + " channels=" +
                        std::to_string(channels) + " block=" + std::to_string(block_rows) +
                        " input=" + std::to_string(channel);
                    for (const auto threshold : identity_thresholds) {
                        const auto plan = suffice::builtin_program("identity", threshold, channel).compile(ranges);
                        const auto expected = reference_mask(fixture, threshold, [&](auto row) {
                            return fixture.sample(row, channel);
                        });
                        check_engines(fixture, plan, expected, "identity", context + " identity t=" +
                                      std::to_string(threshold), true);
                    }
                    for (const auto threshold : std::array<i64, 3>{{0, 100000000, 4294967295LL}}) {
                        const auto plan = suffice::builtin_program("delta", threshold, channel).compile(ranges);
                        const auto expected = reference_mask(fixture, threshold, [&](auto row) {
                            return absolute(fixture.sample(row, channel) - fixture.sample(row, channel, -1));
                        });
                        check_engines(fixture, plan, expected, "delta", context + " delta");
                    }
                    for (const auto threshold : std::array<i64, 3>{{0, 4294967295LL, 8589934590LL}}) {
                        const auto plan = suffice::builtin_program("fir3", threshold, channel).compile(ranges);
                        const auto expected = reference_mask(fixture, threshold, [&](auto row) {
                            return absolute(fixture.sample(row, channel, -1) -
                                            2 * fixture.sample(row, channel) + fixture.sample(row, channel, 1));
                        });
                        check_engines(fixture, plan, expected, "fir3", context + " fir3");
                    }
                    for (const auto threshold : std::array<i64, 3>{{0, 10000000000LL, 4611686018427387904LL}}) {
                        const auto plan = suffice::builtin_program("energy", threshold, channel).compile(ranges);
                        const auto expected = reference_mask(fixture, threshold, [&](auto row) {
                            const auto x = fixture.sample(row, channel);
                            return x * x;
                        });
                        check_engines(fixture, plan, expected, "energy", context + " energy");
                    }
                }
            }
        }
    }
}

void custom_programs() {
    for (const auto rows : {1u, 31u, 32u, 33u, 65u, 257u}) {
        for (const auto block : {32u, 64u, 128u}) {
            Fixture fixture(rows, 2, block);
            const auto ranges = fixture.ranges();
            const auto source = suffice::parse_program(
                "input a 0 -16; input b 1 16; input c 0 0; input d 1 -1; "
                "scale twice a 2; sub difference twice b; min low c d; "
                "add combined difference low; abs magnitude combined; output magnitude > 2147483647");
            const auto plan = source.compile(ranges);
            const auto expected = reference_mask(fixture, 2147483647, [&](auto row) {
                return absolute(2 * fixture.sample(row, 0, -16) - fixture.sample(row, 1, 16) +
                                std::min(fixture.sample(row, 0), fixture.sample(row, 1, -1)));
            });
            check_engines(fixture, plan, expected, {}, "custom wide halo");

            // Test a program whose only load is displaced to one side of the
            // output. The runtime must not assume min_offset<=0<=max_offset.
            for (const int offset : {-16, 16}) {
                suffice::Program p;
                p.output(p.load(1, offset), -1);
                const auto shifted = p.compile(ranges);
                const auto shifted_expected = reference_mask(fixture, -1, [&](auto row) {
                    return fixture.sample(row, 1, offset);
                });
                check_engines(fixture, shifted, shifted_expected, {}, "shifted-only input");
            }

            for (const auto threshold : {-1, 0}) {
                suffice::Program p;
                const auto x = p.load(1, -16);
                p.output(p.sub(x, x), threshold);
                const auto constant = p.compile(ranges);
                require(constant.loads().empty(), "cancellation retained input load");
                const auto zeros = reference_mask(fixture, threshold, [](auto) { return i64{0}; });
                check_engines(fixture, constant, zeros, {}, "constant cancellation");
            }
        }
    }
}

void rejected_contracts() {
    Fixture fixture(65, 1, 32);
    const auto valid = suffice::builtin_program("identity", 0).compile(fixture.ranges());
    const std::array<std::vector<unsigned>, 8> invalid{{
        {}, {0, 32}, {4, 4, 32}, {16, 8, 32}, {4, 16}, {4, 33}, {33}, {1, 2, 31}
    }};
    for (const auto& stages : invalid) {
        must_throw([&] {
            suffice::ExecutionOptions options;
            options.stages = stages;
            suffice::ProgressiveExecutor bad(*fixture.packed, valid, options);
        }, "invalid precision stage schedule accepted");
    }
    const std::array<Bounds, 1> narrow_range{{{-1, 1}}};
    const auto narrow = suffice::builtin_program("identity", 0).compile(narrow_range);
    must_throw([&] { suffice::ProgressiveExecutor bad(*fixture.packed, narrow); },
               "narrow compiled range accepted by progressive constructor");
    std::vector<std::uint64_t> mask(suffice::mask_words(fixture.rows));
    std::vector<std::int32_t> scratch(fixture.packed->info().block_rows * 3);
    const CompiledProgram uncompiled;
    must_throw([&] { suffice::ProgressiveExecutor bad(*fixture.packed, uncompiled); },
               "uncompiled plan accepted by progressive constructor");
    must_throw([&] { suffice::DenseExecutor bad(*fixture.raw, uncompiled); },
               "uncompiled plan accepted by dense constructor");
    must_throw([&] { (void)suffice::run_packed_full(*fixture.packed, uncompiled, mask, scratch); },
               "uncompiled plan accepted by packed-full executor");
    must_throw([&] { (void)suffice::run_dense(*fixture.raw, narrow, mask); },
               "narrow compiled range accepted by dense executor");
    must_throw([&] { (void)suffice::run_packed_full(*fixture.packed, narrow, mask, scratch); },
               "narrow compiled range accepted by packed-full executor");

    const std::array<Bounds, 2> two_ranges{{
        {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()},
        {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}
    }};
    const auto wrong_channel = suffice::builtin_program("identity", 0, 1).compile(two_ranges);
    must_throw([&] { suffice::ProgressiveExecutor bad(*fixture.packed, wrong_channel); },
               "invalid channel accepted by progressive constructor");
    must_throw([&] { (void)suffice::run_dense(*fixture.raw, wrong_channel, mask); },
               "invalid channel accepted by dense executor");
    must_throw([&] { (void)suffice::run_packed_full(*fixture.packed, wrong_channel, mask, scratch); },
               "invalid channel accepted by packed-full executor");

    std::vector<std::uint64_t> wrong_mask(mask.size() + 1);
    suffice::ProgressiveExecutor engine(*fixture.packed, valid);
    must_throw([&] { (void)engine.run(wrong_mask); }, "progressive output size not validated");
    must_throw([&] { (void)suffice::run_dense(*fixture.raw, valid, wrong_mask); },
               "dense output size not validated");
    must_throw([&] { (void)suffice::run_packed_full(*fixture.packed, valid, wrong_mask, scratch); },
               "packed-full output size not validated");
    must_throw([&] { (void)suffice::run_packed_full(*fixture.packed, valid, mask, {}); },
               "packed-full scratch size not validated");

    const auto ordinary = suffice::builtin_program("delta", 0).compile(fixture.ranges());
    require(!suffice::is_fused_builtin(ordinary, "identity"), "wrong builtin specialization accepted");
    require(suffice::is_fused_builtin(ordinary, "delta"), "matching builtin specialization rejected");
    const auto expected = reference_mask(fixture, 0, [&](auto row) {
        return absolute(fixture.sample(row, 0) - fixture.sample(row, 0, -1));
    });
    (void)suffice::run_dense(*fixture.raw, ordinary, mask, "identity", true);
    require(mask == expected, "wrong builtin name changed results instead of using generic fallback");
}

void prepared_execution_allocations() {
    // Establish that the instrumentation sees a real allocation before using
    // zero as evidence about the prepared runtime paths.
    allocation_probe::start();
    void* allocation = nullptr;
    try { allocation = ::operator new(128); }
    catch (...) { (void)allocation_probe::stop(); throw; }
    const auto observed = allocation_probe::stop();
    ::operator delete(allocation);
    require(observed >= 1, "allocation instrumentation did not observe operator new");
    for (const auto channels : {1u, 2u}) {
        Fixture fixture(257, channels, 64);
        for (const auto name : {"identity", "fir3", "energy"}) {
            const auto plan = suffice::builtin_program(name, 100000000).compile(fixture.ranges());
            suffice::ProgressiveExecutor progressive(*fixture.packed, plan);
            suffice::DenseExecutor dense(*fixture.raw, plan, name, true);
            suffice::DenseExecutor lowered(*fixture.raw, plan, name, true, true);
            std::vector<std::uint64_t> mask(suffice::mask_words(fixture.rows));
            std::vector<std::int32_t> scratch(fixture.packed->info().block_rows * channels * 3);
            std::array<ExecutionStats, 4> statistics;

            // Construction, validation, allocation and first-use initialization
            // precede measurement. The public prepared execution paths must not
            // allocate C++ heap storage on subsequent calls.
            (void)progressive.run(mask);
            (void)dense.run(mask);
            (void)suffice::run_packed_full(*fixture.packed, plan, mask, scratch);
            (void)lowered.run(mask);
            allocation_probe::start();
            try {
                for (int repetition = 0; repetition < 3; ++repetition) {
                    statistics[0] = progressive.run(mask);
                    statistics[1] = dense.run(mask);
                    statistics[2] = suffice::run_packed_full(*fixture.packed, plan, mask, scratch);
                    statistics[3] = lowered.run(mask);
                }
            } catch (...) {
                (void)allocation_probe::stop();
                throw;
            }
            const auto allocations = allocation_probe::stop();
            require(allocations == 0, std::string("prepared execution allocated heap memory: ") + name);
            require(statistics[0].accepted == statistics[1].accepted &&
                    statistics[0].accepted == statistics[2].accepted &&
                    statistics[0].accepted == statistics[3].accepted,
                    "allocation measurement engines disagreed");
        }
    }
}

void prepared_dense_preimage_programs() {
    // 65 rows exercise vector tails and a partially populated output word.
    // The deterministic fixture contains exact ties at 0, +/-1, and +/-100000.
    for (const auto channels : {1u, 2u}) {
        Fixture fixture(65, channels, 64);
        const auto ranges = fixture.ranges();
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            for (unsigned kind = 0; kind < 6; ++kind) {
                std::array<i64, 4> thresholds{};
                switch (kind) {
                case 0: thresholds = {-10, -7, -4, 299993}; break; // 3*x-7
                case 1: thresholds = {4, 7, 10, 300007}; break;   // abs(3*x-7)
                case 2: thresholds = {-1, 0, 1, 10000000000LL}; break;
                case 3: thresholds = {-100001, -100000, 0, 100000}; break;
                case 4: thresholds = {-1, -1, -1, -1}; break; // max(x,-x) always >=0
                default: thresholds = {0, 0, 0, 0}; break;    // min(x,-x) always <=0
                }
                const auto cases = kind < 4 ? thresholds.size() : std::size_t{1};
                for (std::size_t test = 0; test < cases; ++test) {
                    suffice::Program source;
                    const auto x = source.load(channel);
                    auto score = x;
                    if (kind == 0 || kind == 1) {
                        score = source.sub(source.mul_const(x, 3), source.constant(7));
                        if (kind == 1) score = source.abs(score);
                    } else if (kind == 2) score = source.square(x);
                    else if (kind == 3)
                        score = source.max(source.min(x, source.constant(100000)), source.constant(-100000));
                    else if (kind == 4) score = source.max(x, source.mul_const(x, -1));
                    else score = source.min(x, source.mul_const(x, -1));
                    source.output(score, thresholds[test]);
                    const auto plan = source.compile(ranges);
                    const auto preimage = suffice::compile_preimage(plan, ranges[channel]);
                    require(preimage.complete, "targeted dense predicate was not preimage eligible");
                    if (kind == 4) {
                        require(preimage.count == 1 && preimage.accepted[0] == ranges[channel],
                                "all-accepted inverse image was not the complete domain");
                        require(plan.decide(plan.output_bounds()) == suffice::Decision::Unknown,
                                "all-accepted case did not require correlation-sensitive lowering");
                    } else if (kind == 5) {
                        require(preimage.count == 0, "all-rejected inverse image was not empty");
                        require(plan.decide(plan.output_bounds()) == suffice::Decision::Unknown,
                                "all-rejected case did not require correlation-sensitive lowering");
                    }
                    const auto expected = reference_mask(fixture, thresholds[test], [&](auto row) {
                        const i64 value = fixture.sample(row, channel);
                        if (kind == 0) return 3 * value - 7;
                        if (kind == 1) return absolute(3 * value - 7);
                        if (kind == 2) return value * value;
                        if (kind == 3) return std::clamp<i64>(value, -100000, 100000);
                        if (kind == 4) return std::max(value, -value);
                        return std::min(value, -value);
                    });
                    for (const bool allow_simd : {true, false}) {
                        suffice::DenseExecutor lowered(*fixture.raw, plan, {}, allow_simd, true);
                        std::vector<std::uint64_t> actual(expected.size(), ~std::uint64_t{0});
                        auto stats = lowered.run(actual);
                        require(actual == expected, "targeted dense preimage differs from independent oracle");
                        check_statistics(stats, actual, fixture.rows, "targeted dense preimage");
                        // Repeat a prepared run under the allocation probe.
                        // Stop instrumentation before assertions can allocate.
                        std::fill(actual.begin(), actual.end(), ~std::uint64_t{0});
                        allocation_probe::start();
                        try { stats = lowered.run(actual); }
                        catch (...) { (void)allocation_probe::stop(); throw; }
                        const auto allocations = allocation_probe::stop();
                        require(allocations == 0, "targeted prepared dense preimage allocated heap memory");
                        require(actual == expected, "targeted prepared dense preimage replay differs");
                        check_statistics(stats, actual, fixture.rows, "targeted dense preimage replay");
                    }
                }
            }
        }
    }
}

} // namespace

void test_runtime() {
    builtin_matrix();
    custom_programs();
    rejected_contracts();
    prepared_execution_allocations();
    prepared_dense_preimage_programs();
}
