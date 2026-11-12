#pragma once

#include "suffice/ir.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace suffice {

struct PredicatePreimage {
    // A complete result is an exact partition of the entire requested domain.
    // Accepted intervals are inclusive, sorted, disjoint, and nonadjacent.
    bool complete{};
    std::array<Bounds, 8> accepted{};
    std::size_t count{};
    std::uint32_t visited{};
    std::string reason;
};

// Compile score(x)>threshold into accepted input intervals. Supports exactly
// one unique unshifted input. The domain must be a nonempty int32 interval
// contained within that input's certified range. Unsupported cases and bounded
// search failures return complete=false, count=0, and a zeroed accepted array;
// callers must retain their existing exact execution path in those cases.
[[nodiscard]] PredicatePreimage compile_preimage(
    const CompiledProgram& program, Bounds domain, std::uint32_t node_budget = 4096);

} // namespace suffice
