#pragma once

#include "suffice/predicate.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace suffice {

inline constexpr std::uint32_t max_wordcode_nodes = 16384;
inline constexpr std::uint32_t default_wordcode_node_budget = 4096;
inline constexpr std::uint32_t no_wordcode_slot = UINT32_MAX;

enum class WordcodeOp : std::uint8_t { False, True, Plane, And, Or, Not };

struct WordcodeNode {
    WordcodeOp op{WordcodeOp::False};
    std::uint32_t a{}, b{};
    std::uint16_t load_index{};
    // Significance-plane index: 0 is the biased most-significant/sign plane.
    std::uint8_t bit{};
    // Only Boolean operations own a reusable arena slot. Leaves are views.
    std::uint32_t slot{no_wordcode_slot};
};

// Complete, topologically ordered AND/OR/NOT circuit over exact input planes.
// Constants and planes require no materialized intermediate vector. Load
// certificates remain even when range folding removes their last plane. A failed
// lowering exposes no nodes, loads or executable root. All range certificates
// must hold for the same immutable packed input used by the executor.
struct WordcodePlan {
    bool complete{};
    std::string reason;
    std::vector<WordcodeNode> nodes;
    std::vector<Load> loads;
    std::uint32_t root{}, plane_count{}, operation_count{}, live_slot_count{};
    std::uint32_t source_predicate_nodes{}, source_comparisons{};
    unsigned maximum_depth{};
    [[nodiscard]] std::string describe() const;
};

// For u = uint32(x) xor 0x80000000 and k the biased constant, start g=false;
// for bits i=0..31: g = k_i ? (u_i & g) : (u_i | g). Trailing one bits of k
// disappear, known constant input planes are folded, and shared subcircuits
// are hash-consed. The limit bounds intermediate as well as final node count.
[[nodiscard]] WordcodePlan compile_wordcode(
    const PredicatePlan&, std::uint32_t node_budget = default_wordcode_node_budget);

class WordcodeExecutor {
public:
    // Input payload must already be verified. The immutable dataset and plan
    // must outlive the prepared executor. run/run_range allocate no heap memory.
    WordcodeExecutor(const PackedDataset&, const WordcodePlan&,
                     std::size_t tile_words = 256, bool allow_simd = true);
    ExecutionStats run(std::span<std::uint64_t> mask);
    // Output bit zero corresponds to global first_row; arbitrary unaligned
    // slices preserve original block identities and contain no repacked input.
    ExecutionStats run_range(std::uint64_t first_row, std::uint64_t row_count,
                             std::span<std::uint64_t> mask);
    [[nodiscard]] std::uint64_t scratch_bytes() const noexcept;
    [[nodiscard]] static std::uint64_t required_scratch_bytes(
        const WordcodePlan&, std::size_t tile_words = 256) noexcept;
    [[nodiscard]] std::string description() const;
    [[nodiscard]] bool uses_simd() const noexcept { return use_simd_; }

private:
    struct View {
        const std::uint8_t* bytes{};
        const std::uint64_t* native_words{};
        std::size_t size{};
        unsigned shift{};
        std::uint64_t constant{};
    };
    const PackedDataset& data_;
    const WordcodePlan& plan_;
    std::size_t tile_words_{};
    bool use_simd_{};
    std::vector<std::uint64_t> arena_;
    std::vector<View> views_;
    // Indexed by node id; populated only for Plane nodes once per input block.
    std::vector<std::span<const std::uint8_t>> planes_;
    [[nodiscard]] static std::uint64_t word(const View&, std::size_t) noexcept;
};

} // namespace suffice
