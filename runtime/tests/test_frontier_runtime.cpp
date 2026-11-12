#include "suffice/frontier_runtime.hpp"
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
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("frontier runtime: ") + message);
}
template<class F> void rejects(F&& f, const char* message) {
    bool failed = false;
    try { f(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::PackedDataset> packed;
    Fixture(std::uint64_t rows, std::uint32_t block_rows,
            unsigned dirty_words = UINT32_MAX) {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            directory = std::filesystem::temp_directory_path() /
                ("suffice-frontier-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence++));
            if (std::filesystem::create_directory(directory)) break;
            if (attempt == 31) throw std::runtime_error("cannot create frontier fixture");
        }
        constexpr std::array<std::int32_t, 13> edges{{
            INT32_MIN, INT32_MAX, -1001, -1000, -999, -1, 0, 1, 999, 1000, 1001, -1048576, 1048575}};
        values.resize(static_cast<std::size_t>(rows) * 2);
        std::uint32_t state = 0x51505354;
        for (std::size_t i = 0; i < values.size(); ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            // Half the words mix equal-prefix threshold boundaries; the rest
            // cover the complete signed domain and exercise early resolution.
            values[i] = (i / 128) % 2 == 0 ? edges[(i * 7) % edges.size()] : std::bit_cast<std::int32_t>(state);
            if (dirty_words != UINT32_MAX) {
                const auto row = i / 2;
                values[i] = row / 64 < dirty_words && row % 64 < 2
                    ? 1000 + static_cast<std::int32_t>(row % 2) : 1000000000;
            }
        }
        std::ofstream raw(directory / "input.i32", std::ios::binary);
        for (const auto value : values) {
            const auto u = std::bit_cast<std::uint32_t>(value);
            for (unsigned b = 0; b < 4; ++b) raw.put(static_cast<char>((u >> (b * 8)) & 255U));
        }
        raw.close(); require(bool(raw), "fixture raw write failed");
        (void)suffice::pack_file(directory / "input.i32", directory / "input.sfp", 2, block_rows);
        packed = std::make_unique<suffice::PackedDataset>(directory / "input.sfp");
        packed->verify_payload();
    }
    ~Fixture() {
        packed.reset();
        std::error_code ec; std::filesystem::remove_all(directory, ec);
    }
    std::vector<suffice::Bounds> ranges() const {
        std::vector<suffice::Bounds> result;
        for (const auto range : packed->info().ranges) result.push_back({range.min, range.max});
        return result;
    }
};

suffice::Program source_program() {
    suffice::Program p;
    const auto x = p.load(0), y = p.load(1);
    p.output(p.min(p.abs(x), p.abs(y)), 1000);
    return p;
}

std::vector<std::uint64_t> reference(const Fixture& f, const suffice::CompiledProgram& p) {
    const auto rows = f.packed->info().rows;
    std::vector<std::uint64_t> output(suffice::mask_words(rows));
    std::array<suffice::i64, suffice::max_program_nodes> input{};
    for (std::uint64_t row = 0; row < rows; ++row) {
        for (std::size_t i = 0; i < p.loads().size(); ++i)
            input[i] = f.values[static_cast<std::size_t>(row) * 2 + p.loads()[i].channel];
        const bool actual = p.evaluate_exact(std::span<const suffice::i64>(input.data(), p.loads().size())) > p.threshold();
        const auto x = static_cast<suffice::i64>(f.values[static_cast<std::size_t>(row) * 2]);
        const auto y = static_cast<suffice::i64>(f.values[static_cast<std::size_t>(row) * 2 + 1]);
        const bool independent = (x < -1000 || x > 1000) && (y < -1000 || y > 1000);
        require(actual == independent, "original IR differs from integer formula");
        if (actual) output[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64);
    }
    return output;
}

void check_range(suffice::FrontierExecutor& executor, std::span<const std::uint64_t> expected,
                 std::uint64_t first, std::uint64_t count, unsigned depth) {
    std::vector<std::uint64_t> guarded(suffice::mask_words(count) + 2, UINT64_MAX);
    auto output = std::span<std::uint64_t>(guarded).subspan(1, guarded.size() - 2);
    allocation_probe::start();
    suffice::ExecutionStats stats;
    try { stats = executor.run_range(first, count, output); }
    catch (...) { allocation_probe::stop(); throw; }
    require(allocation_probe::stop() == 0, "prepared ranged call allocated");
    require(guarded.front() == UINT64_MAX && guarded.back() == UINT64_MAX, "output guard overwritten");
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto wanted = (expected[static_cast<std::size_t>((first + i) / 64)] >> ((first + i) % 64)) & 1U;
        require(((output[static_cast<std::size_t>(i / 64)] >> (i % 64)) & 1U) == wanted,
                "global range differs from independent full result");
    }
    if (count % 64) require((output.back() >> (count % 64)) == 0, "nonzero output padding");
    require(stats.rows == count && stats.accepted == suffice::mask_popcount(output), "wrong row/count accounting");
    require(stats.output_bytes == output.size_bytes(), "wrong output-byte accounting");
    require(stats.frontier_prefix_resolved_rows + stats.refined_rows == count, "prefix/refinement partition is wrong");
    require(stats.frontier_dirty_words <= stats.frontier_fallback_words,
            "physical repair undercounts dirty words");
    require(stats.frontier_sparse_words <= stats.frontier_dirty_words,
            "sparse repairs exceed dirty words");
    require(stats.refined_rows <= stats.frontier_fallback_rows && stats.frontier_fallback_rows <= count,
            "physical repair rows do not contain logical uncertainty");
    require(std::accumulate(stats.resolved_by_bits.begin(), stats.resolved_by_bits.end(), std::uint64_t{0}) == count,
            "resolution histogram is not a partition");
    if (depth == 32) require(!stats.frontier_fallback_words && !stats.refined_rows, "full prefix did not resolve every row");
}
} // namespace

void test_frontier_runtime() {
    for (const auto rows : {0u, 1u, 33u, 513u, 8193u}) {
        for (const auto block : {32u, 256u, 4096u}) {
            Fixture f(rows, block);
            const auto compiled = source_program().compile(f.ranges());
            const auto predicate = suffice::compile_predicate(compiled);
            const auto expected = reference(f, compiled);
            for (const auto depth : {0u, 1u, 8u, 12u, 16u, 31u, 32u}) {
                const auto plan = suffice::compile_frontier(predicate, depth);
                require(plan.complete, "valid prefix lowering failed");
                for (const auto policy : {suffice::FrontierRepairPolicy::SparseWords,
                                          suffice::FrontierRepairPolicy::Hybrid,
                                          suffice::FrontierRepairPolicy::DenseTiles}) {
                const suffice::FrontierRepairOptions options{policy, 25};
                for (const bool allow_simd : {false, true}) {
                    // Odd-sized batches exercise operator tails separately
                    // from physical block and output-mask tails.
                    const auto tile = allow_simd ? 17u : 5u;
                    suffice::FrontierExecutor executor(*f.packed, plan, tile, allow_simd, options);
                    require(executor.scratch_bytes() <= executor.required_scratch_bytes(plan, tile, options), "workspace model undercounts");
                    if (!allow_simd) require(!executor.uses_simd(), "forced scalar still reports SIMD");
                    check_range(executor, expected, 0, rows, depth);
                    for (const auto first : {0u, 1u, 7u, 8u, 31u, 32u, 63u, 64u, 127u, rows}) {
                        if (first > rows) continue;
                        for (const auto count : {0u, 1u, 7u, 63u, 64u, 65u, 257u})
                            check_range(executor, expected, first, std::min(count, rows - first), depth);
                    }
                    std::array<std::uint64_t, 1> one{};
                    rejects([&] { executor.run_range(rows + 1, 0, {}); }, "past-end empty range accepted");
                    rejects([&] { executor.run_range(rows, 1, one); }, "past-end range accepted");
                    rejects([&] { executor.run_range(0, UINT64_MAX, one); }, "overflowing row count accepted");
                    rejects([&] { executor.run_range(0, 0, one); }, "wrong output shape accepted");
                }
                }
            }
        }
    }
    Fixture broad(513, 256);
    const auto predicate = suffice::compile_predicate(source_program().compile(broad.ranges()));
    const auto plan = suffice::compile_frontier(predicate, 12);
    rejects([&] { suffice::FrontierExecutor x(*broad.packed, plan, 0); }, "zero tile accepted");
    rejects([&] { suffice::FrontierExecutor x(*broad.packed, plan, 4097); }, "oversized tile accepted");
    for (const auto percent : {0u, 101u, UINT32_MAX}) {
        const suffice::FrontierRepairOptions bad{suffice::FrontierRepairPolicy::Hybrid, percent};
        rejects([&] { suffice::FrontierExecutor x(*broad.packed, plan, 16, true, bad); }, "invalid density accepted");
        require(!suffice::FrontierExecutor::required_scratch_bytes(plan, 16, bad), "invalid density has workspace estimate");
    }
    rejects([&] { suffice::FrontierExecutor x(*broad.packed, plan, 16, true,
        {static_cast<suffice::FrontierRepairPolicy>(255), 25}); }, "invalid policy accepted");
    require(suffice::parse_frontier_repair_policy("sparse") == suffice::FrontierRepairPolicy::SparseWords,
            "sparse policy parse failed");
    require(suffice::frontier_repair_policy_name(suffice::parse_frontier_repair_policy("hybrid")) == "hybrid",
            "hybrid policy round trip failed");
    rejects([] { (void)suffice::parse_frontier_repair_policy("unknown"); }, "unknown repair policy accepted");
    auto changed = plan;
    changed.prefix.must_root = changed.prefix.may_root;
    require(plan.prefix.must_root != plan.prefix.may_root, "tamper fixture has no uncertainty");
    rejects([&] { suffice::FrontierExecutor x(*broad.packed, changed); }, "modified bound accepted");
    const std::array<suffice::Bounds, 2> narrow{{{0, 10}, {0, 10}}};
    const auto narrow_plan = suffice::compile_frontier(suffice::compile_predicate(source_program().compile(narrow)));
    require(narrow_plan.complete, "constant narrow plan failed");
    rejects([&] { suffice::FrontierExecutor x(*broad.packed, narrow_plan); }, "eliminated source certificate was lost");

    // A controlled 16-word tile exercises both sides of the inclusive25% rule.
    for (const auto dirty : {1u, 3u, 4u, 16u}) {
        Fixture f(1024, 4096, dirty);
        const auto cp = source_program().compile(f.ranges());
        const auto fp = suffice::compile_frontier(suffice::compile_predicate(cp), 12);
        const auto expected = reference(f, cp);
        for (const auto policy : {suffice::FrontierRepairPolicy::SparseWords,
                                  suffice::FrontierRepairPolicy::Hybrid,
                                  suffice::FrontierRepairPolicy::DenseTiles}) {
            suffice::FrontierExecutor executor(*f.packed, fp, 16, true, {policy, 25});
            std::vector<std::uint64_t> output(suffice::mask_words(1024));
            const auto stats = executor.run(output);
            require(output == expected, "controlled density mask differs");
            require(stats.frontier_dirty_words == dirty && stats.refined_rows == 2 * dirty,
                    "controlled density logical counters differ");
            const bool dense = policy == suffice::FrontierRepairPolicy::DenseTiles ||
                (policy == suffice::FrontierRepairPolicy::Hybrid && dirty >= 4);
            require(stats.frontier_dense_tiles == (dense ? 1u : 0u), "density dispatch differs");
            require(stats.frontier_fallback_words == (dense ? 16u : dirty), "physical word count differs");
            require(stats.frontier_fallback_rows == (dense ? 1024u : dirty * 64), "physical row count differs");
            require(stats.frontier_sparse_words == (dense ? 0u : dirty), "sparse word count differs");
        }
        const auto full = suffice::compile_frontier(suffice::compile_predicate(cp), 32);
        require(full.prefix.must_root == full.prefix.may_root, "full prefix roots differ");
        suffice::FrontierExecutor full_sparse(*f.packed, full, 16, true,
                                             {suffice::FrontierRepairPolicy::SparseWords, 25});
        suffice::FrontierExecutor full_hybrid(*f.packed, full, 16, true);
        require(full_sparse.scratch_bytes() == full_hybrid.scratch_bytes(),
                "unreachable fallback retained hybrid storage");
        check_range(full_hybrid, expected, 0, 1024, 32);
    }
    Fixture constant(513, 256, 0);
    const auto constant_plan = suffice::compile_frontier(
        suffice::compile_predicate(source_program().compile(constant.ranges())));
    suffice::FrontierExecutor constant_executor(*constant.packed, constant_plan);
    require(constant_executor.scratch_bytes() == sizeof(suffice::FrontierExecutor),
            "constant plan retains execution buffers");
}
