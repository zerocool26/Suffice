#pragma once

#include "suffice/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace suffice {

// Evaluate the original integer DAG in bounded tiles. A lifetime pass assigns
// vector slots; dying inputs can become an instruction's output in place.
// Inputs and plan must remain immutable and outlive the prepared executor.
// All allocation, range checking, and slot assignment happen at construction.
class VectorExecutor {
public:
    // The trailing flag explicitly guarantees already-verified immutable raw
    // values within all compiled load ranges. The default performs the scan.
    VectorExecutor(const RawDataset&, const CompiledProgram&, std::size_t tile_rows = 256, bool allow_simd = true,
                   bool inputs_already_verified = false);
    VectorExecutor(const PackedDataset&, const CompiledProgram&, std::size_t tile_rows = 256, bool allow_simd = true);
    ExecutionStats run(std::span<std::uint64_t> output_mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output_mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] std::string description() const;
    [[nodiscard]] std::size_t slot_count() const noexcept { return slot_count_; }
    [[nodiscard]] std::size_t tile_rows() const noexcept { return tile_rows_; }
    [[nodiscard]] bool uses_simd() const noexcept { return use_simd_; }

private:
    const RawDataset* raw_{};
    const PackedDataset* packed_{};
    const CompiledProgram& plan_;
    std::size_t tile_rows_{};
    std::size_t slot_count_{};
    bool use_simd_{};
    std::array<std::uint16_t, max_program_nodes> slots_{};
    std::array<std::uint16_t, max_program_nodes> load_channels_{};
    std::vector<std::uint32_t> channels_;
    std::vector<i64> arena_;
    std::vector<std::int32_t> decoded_;
    std::array<std::uint64_t, 3> resident_{};
    void prepare();
    void decode_region(std::uint64_t block, ExecutionStats&);
};

} // namespace suffice
