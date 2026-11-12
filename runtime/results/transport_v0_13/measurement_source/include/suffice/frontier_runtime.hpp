#pragma once

#include "suffice/frontier.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace suffice {

enum class FrontierRepairPolicy : std::uint8_t { SparseWords, Hybrid, DenseTiles };

struct FrontierRepairOptions {
    FrontierRepairPolicy policy{FrontierRepairPolicy::Hybrid};
    // Predeclared heuristic: at least one dirty word per AVX2-width group.
    // This is not a hardware-independent break-even guarantee.
    unsigned dense_threshold_percent{25};
};

[[nodiscard]] std::string_view frontier_repair_policy_name(FrontierRepairPolicy) noexcept;
[[nodiscard]] FrontierRepairPolicy parse_frontier_repair_policy(std::string_view);

// Runs a certified prefix forest in word batches, then evaluates the exact
// circuit for dirty words or, at sufficient density, complete SIMD tiles. No external
// host compiler is needed. Input payload must already have been verified.
// The immutable dataset and plan must outlive this executor. Output storage
// must not overlap either of them. One executor cannot run concurrently.
class FrontierExecutor {
public:
    FrontierExecutor(const PackedDataset&, const FrontierPlan&,
                     std::size_t tile_words = 128, bool allow_simd = true,
                     FrontierRepairOptions options = {});
    ExecutionStats run(std::span<std::uint64_t> output);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] static std::uint64_t required_scratch_bytes(
        const FrontierPlan&, std::size_t tile_words = 128,
        FrontierRepairOptions options = {}) noexcept;
    [[nodiscard]] bool uses_simd() const noexcept { return use_simd_; }
    [[nodiscard]] std::string description() const;

private:
    struct View {
        const std::uint8_t* bytes{};
        const std::uint64_t* words{};
        std::size_t size{};
        unsigned shift{};
        std::uint64_t constant{};
    };
    const PackedDataset& data_;
    const FrontierPlan& plan_;
    std::size_t tile_words_{};
    bool use_simd_{};
    bool can_refine_{};
    FrontierRepairOptions repair_;
    std::vector<std::uint64_t> arena_, exact_values_;
    std::vector<View> views_;
    std::vector<std::span<const std::uint8_t>> prefix_planes_, exact_planes_;
    std::vector<std::uint32_t> exact_to_prefix_;
    std::unique_ptr<WordcodeExecutor> dense_executor_;
    std::vector<std::uint64_t> dense_output_;
    std::vector<std::uint32_t> dirty_words_;
    [[nodiscard]] static std::uint64_t word(const View&, std::size_t) noexcept;
};

} // namespace suffice
