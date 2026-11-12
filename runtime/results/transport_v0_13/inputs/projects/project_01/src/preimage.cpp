#include "suffice/preimage.hpp"

#include <array>
#include <limits>
#include <utility>

namespace suffice {

PredicatePreimage compile_preimage(const CompiledProgram& program, Bounds domain,
                                   std::uint32_t node_budget) {
    PredicatePreimage result;
    auto incomplete = [&](const char* reason) {
        result.complete = false;
        result.count = 0;
        result.accepted.fill(Bounds{});
        result.reason = reason;
        return std::move(result);
    };
    if (program.node_count() == 0) return incomplete("program is not compiled");
    if (program.loads().size() != 1 || program.loads()[0].offset != 0)
        return incomplete("requires exactly one unique unshifted input");
    if (domain.lo > domain.hi || domain.lo < std::numeric_limits<std::int32_t>::min() ||
        domain.hi > std::numeric_limits<std::int32_t>::max())
        return incomplete("domain must be a nonempty int32 interval");
    const auto input_range = program.loads()[0].range;
    if (domain.lo < input_range.lo || domain.hi > input_range.hi)
        return incomplete("domain exceeds the compiled input range");

    // At most 32 binary splits are possible over an int32 domain. Depth-first
    // traversal keeps one deferred right sibling at each depth, plus the next
    // interval: a maximum of 33 entries. Pushing right before left visits input
    // intervals in increasing order, making interval union a linear operation.
    std::array<Bounds, 33> pending{};
    std::size_t pending_count = 1;
    pending[0] = domain;
    while (pending_count != 0) {
        if (result.visited >= node_budget) return incomplete("node budget exhausted");
        const Bounds candidate = pending[--pending_count];
        ++result.visited;
        auto decision = program.decide(program.evaluate_bounds_unchecked(
            std::span<const Bounds>(&candidate, 1)));
        if (decision == Decision::Unknown && candidate.lo == candidate.hi) {
            const i64 exact = candidate.lo;
            decision = program.evaluate_exact_unchecked(std::span<const i64>(&exact, 1)) >
                       program.threshold() ? Decision::True : Decision::False;
        }
        if (decision == Decision::True) {
            if (result.count != 0 && result.accepted[result.count - 1].hi + 1 == candidate.lo) {
                result.accepted[result.count - 1].hi = candidate.hi;
            } else {
                if (result.count == result.accepted.size())
                    return incomplete("predicate exceeds the eight-interval limit");
                result.accepted[result.count++] = candidate;
            }
        } else if (decision == Decision::Unknown) {
            // The int32 domain guarantees this subtraction and midpoint fit i64.
            const i64 midpoint = candidate.lo + (candidate.hi - candidate.lo) / 2;
            if (pending_count + 2 > pending.size())
                return incomplete("internal subdivision depth limit reached");
            pending[pending_count++] = {midpoint + 1, candidate.hi};
            pending[pending_count++] = {candidate.lo, midpoint};
        }
    }
    result.complete = true;
    result.reason = "complete";
    return result;
}

} // namespace suffice
