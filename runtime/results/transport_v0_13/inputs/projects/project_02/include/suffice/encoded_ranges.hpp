#pragma once

#include "suffice/runtime.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace suffice {

// Exact membership in a union of at most eight inclusive int32 ranges. The
// input channel has no row shift. Construction normalizes against the verified
// dataset domain; run() operates directly on packed planes without decoding
// scalar values and without allocating memory.
class EncodedRangeExecutor {
public:
    EncodedRangeExecutor(const PackedDataset& dataset, std::uint32_t channel,
                         std::span<const Bounds> accepted,
                         unsigned first_stage_bits = 4);

    ExecutionStats run(std::span<std::uint64_t> output_mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output_mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] std::size_t range_count() const noexcept { return range_count_; }

private:
    struct Range {
        std::uint32_t lower{}, upper{};
    };
    const PackedDataset& dataset_;
    std::uint32_t channel_{};
    unsigned first_stage_bits_{};
    std::array<Range, 8> ranges_{};
    std::size_t range_count_{};
    std::size_t words_per_block_{};
    bool accepts_all_{};
    bool sign_projection_{}, invert_projection_{};
    std::vector<std::uint64_t> equal_lower_, greater_lower_;
    std::vector<std::uint64_t> equal_upper_, less_upper_;
    std::vector<std::uint64_t> unknown_, accepted_;
};

} // namespace suffice
