#pragma once

#include "suffice/ir.hpp"
#include "suffice/residual.hpp"

#include <cstdint>
#include <string>

namespace suffice {

enum class EncodedStrategy : std::uint8_t { Eager, Frontier, Residual, GuardedResidual };
enum class EncodedSchedule : std::uint8_t { Legacy, Pressure, BoundsFirst };
enum class EncodedCarryPlacement : std::uint8_t { Eager, Guarded, Recompute };
enum class EncodedPlaneBinding : std::uint8_t { PerPlane, Grouped };
enum class EncodedRepairPolicy : std::uint8_t { Batch256, Word64, Hybrid };
enum class RawLayout : std::uint8_t { Interleaved, Planar };

struct CodegenOptions {
    // One nonreserved ASCII C++ identifier; nested namespaces are not accepted.
    std::string namespace_name{"suffice_generated"};
    bool enable_avx2{true};
    bool lower_predicates{true};
    bool enable_encoded{true};
    // Legacy/eager preserves the 0.3 computation as an explicit control.
    EncodedStrategy encoded_strategy{EncodedStrategy::Eager};
    unsigned encoded_prefix_bits{12};
    EncodedSchedule encoded_schedule{EncodedSchedule::Legacy};
    // Guarded placement schedules carry-only prefix work and aggregate
    // materialization after the root uncertainty branch. It does not promise
    // that the host compiler retains the same instruction placement. Recompute
    // puts carry reconstruction in a non-inlined cold helper, trading prefix
    // rereads for removal of carry state from the source common path.
    EncodedCarryPlacement encoded_carry_placement{EncodedCarryPlacement::Eager};
    // Grouped requests maximal contiguous required bit ranges per physical
    // channel. Dataset::plane_view must return consecutive plane payloads with
    // stride ceil(block_length/8), as PackedDataset does. The default retains
    // one-plane requests for adapters that implement only that contract.
    EncodedPlaneBinding encoded_plane_binding{EncodedPlaneBinding::PerPlane};
    // The 256-row prefix remains vectorized. Word64 repairs only dirty 64-row
    // words; Hybrid uses that path for exactly one dirty word and otherwise
    // repairs the complete vector. Narrow policies require a residual strategy
    // with Recompute carry placement. Scalar execution is unchanged.
    EncodedRepairPolicy encoded_repair_policy{EncodedRepairPolicy::Batch256};
    // Planar raw values are channel-major: values[channel*rows + row].
    RawLayout raw_layout{RawLayout::Interleaved};
    GuardedResidualOptions guarded_residual_options{};
};

struct GeneratedKernel {
    std::string header;
    std::string description;
    bool predicate_lowered{};
    bool avx2_emitted{};
    bool encoded_emitted{};
    bool encoded_avx2_emitted{};
    std::uint32_t source_nodes{};
    std::uint32_t predicate_nodes{};
    std::uint32_t wordcode_nodes{};
    bool encoded_frontier_emitted{};
    bool encoded_residual_emitted{};
    bool encoded_guarded_residual_emitted{};
    bool encoded_guarded_carry_emitted{};
    bool encoded_recompute_carry_emitted{};
    bool encoded_grouped_binding_emitted{};
    bool encoded_narrow_repair_emitted{};
    std::uint32_t encoded_plane_count{}, encoded_binding_groups{}, encoded_binding_calls_per_block{};
    bool encoded_residual_exact_all_lanes{};
    unsigned encoded_prefix_bits{};
    std::uint32_t encoded_prefix_nodes{}, encoded_prefix_planes{};
    std::uint32_t encoded_residual_nodes{}, encoded_residual_planes{}, encoded_residual_carries{};
    std::uint32_t encoded_residual_initial_carries{};
    // Source fragments: named operations, inlined leaves, and externally
    // supplied values. Plane counts count distinct reads in each fragment.
    // Liveness is a structural estimate, never a machine-register/stack bound.
    std::uint32_t encoded_common_nodes{}, encoded_common_planes{}, encoded_common_live_values{};
    std::uint32_t encoded_cold_prefix_nodes{}, encoded_cold_prefix_planes{}, encoded_cold_prefix_live_values{};
    std::uint32_t encoded_proof_attempts{}, encoded_proof_accepted_rewrites{}, encoded_proof_bdd_nodes{};
    std::uint32_t encoded_proof_graph_budget_rejections{};
    std::uint64_t encoded_proof_steps{};
    bool encoded_proof_exhausted{};
    std::string encoded_proof_reason;
    std::string encoded_reason;
};

// Emits a self-contained C++20 header. It contains a validated PreparedKernel,
// fused scalar code and optional runtime-dispatched AVX2 code. Numeric symbols
// are derived from the compiled IR; source-language names are never injected.
// Compilation of the returned source is deliberately a separate, charged step.
[[nodiscard]] GeneratedKernel emit_cpp(const CompiledProgram&, CodegenOptions = {});

} // namespace suffice
