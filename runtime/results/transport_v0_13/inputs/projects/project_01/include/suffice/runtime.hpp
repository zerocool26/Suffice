#pragma once
#include "suffice/ir.hpp"
#include "suffice/storage.hpp"
#include "suffice/preimage.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace suffice {

class EncodedRangeExecutor;

struct ExecutionStats {
    std::uint64_t rows{}, accepted{}, requested_payload_bytes{};
    std::uint64_t estimated_cacheline_bytes{}, interval_evaluations{}, exact_evaluations{};
    std::uint64_t region_loads{}, cache_reuses{}, loaded_planes{}, bulk_resolutions{};
    std::uint64_t refined_rows{}, peak_scratch_bytes{}, output_bytes{};
    // Frontier-specific work accounting. Prefix-certified output lanes are
    // distinct from whole 64-lane words sent through the exact fallback.
    // Dirty words are logical uncertainty; fallback words/rows count physical
    // repair work, including prefix-resolved lanes in a dense repair tile.
    std::uint64_t frontier_prefix_resolved_rows{}, frontier_fallback_words{};
    std::uint64_t frontier_dirty_words{}, frontier_fallback_rows{};
    std::uint64_t frontier_dense_tiles{}, frontier_sparse_words{};
    // For the frontier backend, the index is the maximum requested plane depth
    // of the successful compiled stage (prefix, or prefix plus exact). This is
    // a logical proof-depth histogram, not a measured hardware-traffic counter.
    std::array<std::uint64_t, 33> resolved_by_bits{};
    std::string_view kernel;
};

struct ExecutionOptions {
    std::vector<unsigned> stages{4,8,16,32};
    bool specialize{true};
};

// A prepared executor owns all scratch buffers. run() allocates no work buffers.
// The dataset and compiled program must outlive the executor. This object is not
// thread-safe; independent prepared executors may share an immutable dataset.
class ProgressiveExecutor {
public:
    ProgressiveExecutor(const PackedDataset&, const CompiledProgram&, ExecutionOptions = {});
    ~ProgressiveExecutor();
    ExecutionStats run(std::span<std::uint64_t> output_mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output_mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] std::string lowering_description() const;
private:
    struct Region {
        std::uint64_t block{}, stamp{};
        std::uint32_t channel{}, rows{}, bits{};
        bool valid{}, pinned{};
        Bounds aggregate{};
        std::vector<std::uint32_t> prefix;
    };
    const PackedDataset& data_;
    const CompiledProgram& plan_;
    ExecutionOptions options_;
    std::vector<Region> cache_;
    std::vector<std::uint32_t> channels_;
    std::vector<std::uint8_t> unresolved_;
    std::vector<std::uint8_t> needed_;
    std::vector<std::uint64_t> equal_bits_, greater_bits_;
    std::uint64_t tick_{};
    bool bitset_plan_{};
    PredicatePreimage preimage_;
    std::unique_ptr<EncodedRangeExecutor> range_executor_;
    std::size_t region(std::uint64_t block, std::uint32_t channel, ExecutionStats&);
    void refine(Region&, unsigned bits, ExecutionStats&);
    Bounds at(const Region&, std::uint32_t local) const noexcept;
    std::size_t find_region(std::uint64_t block, std::uint32_t channel) const;
    ExecutionStats run_bitset(std::uint64_t first_row, std::uint64_t row_count,
                              std::span<std::uint64_t> mask);
};

[[nodiscard]] std::size_t mask_words(std::uint64_t rows);
[[nodiscard]] std::uint64_t mask_checksum(std::span<const std::uint64_t> mask) noexcept;
[[nodiscard]] std::uint64_t mask_popcount(std::span<const std::uint64_t> mask) noexcept;
[[nodiscard]] bool avx2_available() noexcept;
[[nodiscard]] bool is_fused_builtin(const CompiledProgram&, std::string_view name) noexcept;

class DenseExecutor {
public:
    // Scans used input channels once to validate the compiled range contract.
    // inputs_already_verified is an explicit caller guarantee that every raw
    // value already satisfies its immutable compiled load ranges; it skips only
    // that scan. Keep preparation outside execution-only benchmark timing.
    DenseExecutor(const RawDataset&, const CompiledProgram&,
                  std::string_view builtin = {}, bool allow_simd = true,
                  bool lower_preimage = false, bool inputs_already_verified = false);
    ExecutionStats run(std::span<std::uint64_t> mask) const;
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> mask) const;
private:
    const RawDataset& data_;
    const CompiledProgram& plan_;
    std::string builtin_;
    bool allow_simd_{};
    PredicatePreimage preimage_;
};

// "identity", "delta", "fir3", and "energy" use fused C++/AVX2 implementations when
// compatible. An empty/other name uses the same checked-compiled IR as demand.
ExecutionStats run_dense(const RawDataset&, const CompiledProgram&,
                         std::span<std::uint64_t> mask,
                         std::string_view builtin = {}, bool allow_simd = true);
// Control comparison: decode every bit of the SAME packed file, then evaluate
// the same IR; scratch is preallocated by the caller.
ExecutionStats run_packed_full(const PackedDataset&, const CompiledProgram&,
                               std::span<std::uint64_t> mask,
                               std::span<std::int32_t> scratch);

} // namespace suffice
