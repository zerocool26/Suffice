#pragma once

#include "suffice/wordcode.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace suffice {

// A shared, topological Boolean forest with two retained outputs. Each row's
// exact predicate F satisfies must <= F <= may. A Plane node's significance
// index is strictly below the parent plan's prefix_bits. Operation slots keep
// BOTH roots live until the complete forest has been evaluated.
struct FrontierStage {
    std::vector<WordcodeNode> nodes;
    // Identical canonical physical-channel certificates/order to exact.loads.
    // Proof-only channels remain even when no prefix plane references them.
    std::vector<Load> loads;
    std::uint32_t must_root{}, may_root{};
    std::uint32_t plane_count{}, operation_count{}, live_slot_count{};
    unsigned maximum_depth{};
};

struct FrontierPlan {
    bool complete{};
    std::string reason;
    unsigned prefix_bits{12};
    FrontierStage prefix;
    WordcodePlan exact;
    [[nodiscard]] std::string describe() const;

private:
    // Construction witness cannot be changed through the public graph fields.
    // validate_frontier recompiles it under the retained bounded budget and
    // compares canonical executable structure, before prepared execution.
    PredicatePlan source_;
    std::uint32_t node_budget_{};
    friend FrontierPlan compile_frontier(const PredicatePlan&, unsigned, std::uint32_t);
    friend void validate_frontier(const FrontierPlan&);
};

// prefix_bits is in 0..32. At 32 bits both outputs are exact. At zero bits a
// nonconstant predicate may remain unknown, but established constant results
// need no data reads. node_budget caps EACH retained exact/prefix graph and
// each temporary one-comparison lowering; partial plans never escape failure.
// The original PredicatePlan and its certificates are not modified.
[[nodiscard]] FrontierPlan compile_frontier(
    const PredicatePlan&, unsigned prefix_bits = 12,
    std::uint32_t node_budget = default_wordcode_node_budget);

// Throws invalid_argument for incomplete or publicly modified executable
// plans. Rebuilds this compiler's canonical bounded output at preparation time;
// this is not an independent formal proof of the compiler. Allocations here
// are preparation costs. A prepared caller must keep the plan immutable after
// validation, just as it must keep the certified input immutable.
void validate_frontier(const FrontierPlan&);

} // namespace suffice
