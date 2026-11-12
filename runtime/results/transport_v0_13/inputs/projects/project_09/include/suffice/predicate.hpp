#pragma once

#include "suffice/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace suffice {

inline constexpr std::size_t max_predicate_nodes = 256;
inline constexpr std::uint32_t max_predicate_lowering_steps = 1024;
using PredicateNodeId = std::uint16_t;
enum class PredicateOp : std::uint8_t { False, True, Greater, And, Or, Not };

struct PredicateNode {
    PredicateOp op{PredicateOp::False};
    PredicateNodeId a{}, b{};
    std::uint16_t load_index{};
    std::int32_t threshold{};
    friend bool operator==(const PredicateNode&, const PredicateNode&) = default;
};

// Complete plans are topologically ordered Boolean DAGs. No node or accepted
// partial graph escapes a failed compilation. Execution loads can be pruned;
// certificates retain every source range assumption, including assumptions that
// made an entire predicate constant. Both must hold on the immutable input.
struct PredicatePlan {
    bool complete{};
    std::string reason;
    std::array<PredicateNode, max_predicate_nodes> nodes{};
    std::array<Load, max_program_nodes> loads{};
    std::array<Load, max_program_nodes> certificates{};
    std::uint16_t node_count{}, load_count{}, certificate_count{};
    PredicateNodeId root{};
    std::uint32_t lowering_steps{};
    [[nodiscard]] std::string describe() const;
};

// Exact backward lowering for min/max, abs/square, and constant affine
// operations. Nonconstant arithmetic combining two inputs and nonzero input
// offsets retain the caller's general fallback. node_budget is in 1..256;
// at most 1024 inverse-threshold demands may be visited.
[[nodiscard]] PredicatePlan compile_predicate(const CompiledProgram&,
                                             std::uint32_t node_budget = max_predicate_nodes);

class EncodedPredicateExecutor {
public:
    // The complete plan and verified immutable dataset must outlive this
    // executor. All reusable storage is allocated here; run allocates nothing.
    EncodedPredicateExecutor(const PackedDataset&, const PredicatePlan&);
    ExecutionStats run(std::span<std::uint64_t> mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output_mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] static std::uint64_t required_scratch_bytes(const PredicatePlan&) noexcept;
    [[nodiscard]] std::string description() const;
private:
    struct SignView {
        const std::uint8_t* bytes{};
        std::size_t size{};
    };
    const PackedDataset& data_;
    const PredicatePlan& plan_;
    std::vector<std::uint64_t> known_, truth_, words_;
    std::vector<std::span<const std::uint8_t>> planes_;
    std::vector<std::uint32_t> words_loaded_, planes_loaded_;
    std::vector<std::uint16_t> sign_slots_;
    std::vector<std::uint64_t> sign_arena_;
    std::vector<SignView> sign_views_;
    std::array<std::uint64_t, 33> depth_seen_{};
    std::uint64_t block_{};
    std::size_t word_{};
    unsigned count_{}, maximum_depth_{};
    bool sign_plan_{};
    ExecutionStats run_sign(std::uint64_t first_row, std::uint64_t row_count,
                            std::span<std::uint64_t> mask);
    std::uint64_t evaluate(PredicateNodeId, std::uint64_t, ExecutionStats&);
    std::uint64_t compare(const PredicateNode&, std::uint64_t, ExecutionStats&);
    std::uint64_t plane_word(std::uint16_t load, unsigned bit, ExecutionStats&);
};

class DensePredicateExecutor {
public:
    // Scans every distinct range contract, including certificates whose loads
    // were pruned, to establish the plan's range contract.
    // The trailing flag is a caller guarantee of that verification on the same
    // immutable data; it skips only the scan. Same graph as encoded.
    DensePredicateExecutor(const RawDataset&, const PredicatePlan&, bool allow_simd = true,
                           bool inputs_already_verified = false);
    ExecutionStats run(std::span<std::uint64_t> mask);
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> output_mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] static std::uint64_t required_scratch_bytes(const PredicatePlan&) noexcept;
    [[nodiscard]] std::string description() const;
private:
    const RawDataset& data_;
    const PredicatePlan& plan_;
    std::vector<std::uint64_t> known_, truth_;
    std::uint64_t first_{};
    unsigned count_{};
    bool use_simd_{};
    std::uint64_t evaluate(PredicateNodeId, std::uint64_t, ExecutionStats&);
    std::uint64_t compare(const PredicateNode&, std::uint64_t, ExecutionStats&);
};

} // namespace suffice
