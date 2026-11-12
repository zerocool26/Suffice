#include "suffice/preimage.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using suffice::Bounds;
using suffice::i64;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("preimage test: ") + message);
}

bool accepts(const suffice::PredicatePreimage& result, i64 value) {
    for (std::size_t i = 0; i < result.count; ++i)
        if (result.accepted[i].lo <= value && value <= result.accepted[i].hi) return true;
    return false;
}

void check_partition(const suffice::CompiledProgram& plan, Bounds domain,
                     const suffice::PredicatePreimage& result) {
    require(result.complete, "supported predicate did not complete");
    require(result.count <= result.accepted.size(), "interval count exceeds capacity");
    for (std::size_t i = 0; i < result.count; ++i) {
        const auto interval = result.accepted[i];
        require(interval.lo <= interval.hi && interval.lo >= domain.lo && interval.hi <= domain.hi,
                "accepted interval falls outside domain");
        if (i != 0)
            require(result.accepted[i - 1].hi + 1 < interval.lo,
                    "accepted intervals overlap, are adjacent, or are not sorted");
    }
    for (i64 value = domain.lo; value <= domain.hi; ++value) {
        const auto exact = plan.evaluate_exact(std::span<const i64>(&value, 1));
        require(accepts(result, value) == (exact > plan.threshold()), "preimage differs from exact predicate");
    }
}

void check_incomplete(const suffice::PredicatePreimage& result, const char* message) {
    require(!result.complete && result.count == 0 && !result.reason.empty(), message);
    for (const auto interval : result.accepted)
        require(interval == Bounds{}, "incomplete search exposed an accepted interval");
}

void exhaustive_programs() {
    const std::array<Bounds, 1> ranges{{{-100, 100}}};
    for (const auto threshold : std::array<i64, 9>{{-1, 0, 1, 4, 16, 100, 625, 9999, 10000}}) {
        const auto plan = suffice::builtin_program("energy", threshold).compile(ranges);
        const auto result = suffice::compile_preimage(plan, ranges[0]);
        check_partition(plan, ranges[0], result);
        require(result.count <= 2, "energy requires no more than two intervals");
    }
    for (i64 threshold = -1; threshold <= 60; ++threshold) {
        suffice::Program source;
        const auto x = source.load(0);
        const auto affine = source.sub(source.mul_const(x, -3), source.constant(7));
        source.output(source.abs(affine), threshold);
        const auto plan = source.compile(ranges);
        check_partition(plan, ranges[0], suffice::compile_preimage(plan, ranges[0]));
    }
    for (i64 threshold = -5; threshold <= 10; ++threshold) {
        const auto source = suffice::parse_program(
            "input x 0 0; const low -3; const high 7; min below x high; "
            "max clipped below low; output clipped > " + std::to_string(threshold));
        const auto plan = source.compile(ranges);
        check_partition(plan, ranges[0], suffice::compile_preimage(plan, ranges[0]));
    }

    // A nonmonotonic polynomial with three distinct accepted components.
    suffice::Program double_well;
    const auto x = double_well.load(0);
    double_well.output(double_well.square(double_well.sub(double_well.square(x),
                                                         double_well.constant(9))), 16);
    const auto plan = double_well.compile(ranges);
    const auto result = suffice::compile_preimage(plan, ranges[0]);
    check_partition(plan, ranges[0], result);
    require(result.count == 3, "nonmonotonic predicate lost a component");
    require(result.accepted[0] == Bounds{-100, -4} && result.accepted[1] == Bounds{-2, 2} &&
            result.accepted[2] == Bounds{4, 100}, "double-well endpoints incorrect");

    // A strict subdomain must produce exactly its own inverse image.
    check_partition(plan, {-5, 3}, suffice::compile_preimage(plan, {-5, 3}));
    for (i64 point = -10; point <= 10; ++point)
        check_partition(plan, {point, point}, suffice::compile_preimage(plan, {point, point}, 1));
}

void full_int32_and_merging() {
    const i64 minimum = std::numeric_limits<std::int32_t>::min();
    const i64 maximum = std::numeric_limits<std::int32_t>::max();
    const std::array<Bounds, 1> ranges{{{minimum, maximum}}};
    for (const auto threshold : std::array<i64, 8>{{
             std::numeric_limits<i64>::min(), minimum, -1, 0, 1, maximum - 1, maximum,
             std::numeric_limits<i64>::max()}}) {
        const auto plan = suffice::builtin_program("identity", threshold).compile(ranges);
        const auto result = suffice::compile_preimage(plan, ranges[0]);
        require(result.complete && result.visited <= 65, "full int32 identity search failed");
        if (threshold >= maximum) require(result.count == 0, "empty predicate was not empty");
        else {
            const i64 first = threshold < minimum ? minimum : threshold + 1;
            require(result.count == 1 && result.accepted[0] == Bounds{first, maximum},
                    "identity interval endpoint or adjacent merging incorrect");
        }
    }
    const auto energy = suffice::builtin_program("energy", 16).compile(ranges);
    const auto result = suffice::compile_preimage(energy, ranges[0]);
    require(result.complete && result.count == 2, "full int32 energy search failed");
    require(result.accepted[0] == Bounds{minimum, -5} && result.accepted[1] == Bounds{5, maximum},
            "full-range energy strict threshold endpoints incorrect");
    require(result.visited <= 255, "energy unexpectedly enumerated its huge domain");
}

suffice::CompiledProgram isolated_points(unsigned count) {
    suffice::Program source;
    const auto x = source.load(0);
    auto distance = source.abs(source.sub(x, source.constant(-8)));
    for (unsigned i = 1; i < count; ++i) {
        const i64 location = -8 + static_cast<i64>(i) * 2;
        distance = source.min(distance, source.abs(source.sub(x, source.constant(location))));
    }
    source.output(source.mul_const(distance, -1), -1);
    return source.compile(std::array<Bounds, 1>{{{-10, 10}}});
}

void bounded_failures() {
    const std::array<Bounds, 1> ranges{{{-8, 8}}};
    suffice::Program source;
    source.output(source.mul_const(source.load(0), -1), 0);
    const auto plan = source.compile(ranges);
    const auto no_work = suffice::compile_preimage(plan, ranges[0], 0);
    check_incomplete(no_work, "zero-budget search incorrectly completed");
    require(no_work.visited == 0, "zero budget performed a visit");
    const auto partial = suffice::compile_preimage(plan, ranges[0], 3);
    check_incomplete(partial, "budget exhaustion incorrectly completed");
    require(partial.visited == 3, "budget limit not respected");
    require(partial.reason.find("budget") != std::string::npos, "budget failure reason missing");
    check_partition(plan, ranges[0], suffice::compile_preimage(plan, ranges[0], 128));

    const auto eight = isolated_points(8);
    const auto supported = suffice::compile_preimage(eight, {-10, 10});
    check_partition(eight, {-10, 10}, supported);
    require(supported.count == 8, "eight-component predicate was not retained");
    const auto fragmented = suffice::compile_preimage(isolated_points(9), {-10, 10});
    check_incomplete(fragmented, "nine-component predicate did not trigger fallback");
    require(fragmented.reason.find("eight") != std::string::npos, "fragmentation failure reason missing");
}

void invalid_and_unsupported() {
    const std::array<Bounds, 1> range{{{-10, 10}}};
    const auto identity = suffice::builtin_program("identity", 0).compile(range);
    for (const auto invalid : std::array<Bounds, 5>{{
             {1, 0}, {-11, 10}, {-10, 11},
             {std::numeric_limits<i64>::min(), 10}, {0, std::numeric_limits<i64>::max()}}})
        check_incomplete(suffice::compile_preimage(identity, invalid), "invalid domain accepted");

    check_incomplete(suffice::compile_preimage(suffice::CompiledProgram{}, range[0]),
                     "uncompiled plan accepted");
    check_incomplete(suffice::compile_preimage(suffice::builtin_program("delta", 0).compile(range), range[0]),
                     "multiple input positions accepted");
    suffice::Program shifted;
    shifted.output(shifted.load(0, 1), 0);
    check_incomplete(suffice::compile_preimage(shifted.compile(range), range[0]),
                     "shifted input accepted");
    suffice::Program constant;
    constant.output(constant.constant(7), 0);
    check_incomplete(suffice::compile_preimage(constant.compile({}), range[0]),
                     "zero-input program accepted");

    // An unshifted input on a nonzero channel is eligible: position, not channel
    // number, controls whether one scalar inverse image describes the output.
    const std::array<Bounds, 2> ranges{{{-1, 1}, {-10, 10}}};
    const auto second = suffice::builtin_program("identity", 0, 1).compile(ranges);
    check_partition(second, ranges[1], suffice::compile_preimage(second, ranges[1]));
}

} // namespace

void test_preimage() {
    exhaustive_programs();
    full_int32_and_merging();
    bounded_failures();
    invalid_and_unsupported();
}
