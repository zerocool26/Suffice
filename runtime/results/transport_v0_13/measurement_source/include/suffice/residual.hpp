#pragma once

#include "suffice/frontier.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace suffice {

inline constexpr std::uint32_t max_residual_carries = 2 * max_predicate_nodes;

enum class ResidualScope : std::uint8_t { AllLanes, UnresolvedLanes };
struct GuardedResidualOptions {
    std::uint32_t bdd_node_budget{65536};
    std::uint64_t proof_step_budget{1000000};
    std::uint32_t rewrite_attempt_budget{256};
    friend bool operator==(const GuardedResidualOptions&, const GuardedResidualOptions&) = default;
};
struct ResidualProofStats {
    std::uint32_t attempts{}, accepted_rewrites{}, bdd_nodes{}, initial_carries{};
    std::uint32_t graph_budget_rejections{};
    std::uint64_t proof_steps{};
    bool exhausted{};
    std::string reason;
    friend bool operator==(const ResidualProofStats&, const ResidualProofStats&) = default;
};

// Carry is a mask already computed by the prefix forest. Plane is always a
// suffix significance index >= prefix_bits; the residual cannot reread prefix
// payload. Constants and leaves do not occupy operation-arena slots.
enum class ResidualOp : std::uint8_t { False, True, Plane, Carry, And, Or, Not };
struct ResidualNode {
    ResidualOp op{ResidualOp::False};
    std::uint32_t a{}, b{};
    std::uint16_t load_index{};
    std::uint8_t bit{};
    std::uint16_t carry_index{};
    std::uint32_t slot{no_wordcode_slot};
    friend bool operator==(const ResidualNode&, const ResidualNode&) = default;
};
struct ResidualStage {
    std::vector<ResidualNode> nodes;
    std::uint32_t root{}, plane_count{}, operation_count{}, live_slot_count{};
    unsigned maximum_depth{};
};
struct ResidualPlan {
    bool complete{};
    std::string reason;
    unsigned prefix_bits{12};
    // UnresolvedLanes permits a different suffix value where may & ~must is
    // false. The whole-result merge remains exact on every valid lane.
    ResidualScope scope{ResidualScope::AllLanes};
    ResidualProofStats proof;
    // All original physical-channel certificates, including proof-only ones,
    // remain in prefix.loads and exact.loads in identical order.
    FrontierStage prefix;
    // Every listed prefix node remains live through completion of the forest,
    // together with must_root/may_root. Roots are deduplicated and constants are
    // rematerialized instead of carried.
    std::vector<std::uint32_t> carry_roots;
    ResidualStage suffix;
    WordcodePlan exact; // Explicit exact control, never called by suffix code.
    [[nodiscard]] std::string describe() const;
private:
    PredicatePlan source_;
    std::uint32_t node_budget_{}, carry_budget_{};
    bool guarded_{};
    GuardedResidualOptions guarded_options_{};
    friend ResidualPlan compile_residual(const PredicatePlan&, unsigned,
                                        std::uint32_t, std::uint32_t);
    friend ResidualPlan compile_guarded_residual(const PredicatePlan&, unsigned,
        std::uint32_t, std::uint32_t, GuardedResidualOptions);
    friend void validate_residual(const ResidualPlan&);
};

// Each graph, including temporary atomic frontier lowerings, is bounded by
// node_budget. carry_budget also bounds temporary carry identities before dead
// carry elimination. An incomplete plan contains no executable partial graph.
// Endpoint depths 0 and 32 are supported explicitly; no shift by 32 occurs.
[[nodiscard]] ResidualPlan compile_residual(
    const PredicatePlan&, unsigned prefix_bits = 12,
    std::uint32_t node_budget = default_wordcode_node_budget,
    std::uint32_t carry_budget = max_residual_carries);

// Carry substitutions are admitted only after a bounded Boolean decision
// diagram proves Q & (old XOR candidate) == false, Q = may & ~must. All plane
// variables are unconstrained, a conservative superset of certified inputs.
// Exhausted optimization budgets retain the last admitted plan (initially the
// all-lane residual). Compiler graph/carry failures still return incomplete.
// Zero proof budgets disable proof work safely. Hard option maxima are
// 262144 BDD nodes, 8000000 proof steps, and 4096 substitution attempts.
[[nodiscard]] ResidualPlan compile_guarded_residual(
    const PredicatePlan&, unsigned prefix_bits = 12,
    std::uint32_t node_budget = default_wordcode_node_budget,
    std::uint32_t carry_budget = max_residual_carries,
    GuardedResidualOptions options = {});

// Checks the stage boundary and reconstructs a canonical bounded witness.
// This catches public-plan mutation; it is not an independent compiler proof.
// Call at preparation; retain immutable plan and certified immutable input.
void validate_residual(const ResidualPlan&);

} // namespace suffice
