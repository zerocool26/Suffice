#include "suffice/ir.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("IR test: ") + message);
}

template<class Exception, class Function>
void require_throws(Function function, const char* message) {
    bool thrown = false;
    try { function(); }
    catch (const Exception&) { thrown = true; }
    require(thrown, message);
}

using suffice::Bounds;
using suffice::Decision;
using suffice::i64;

suffice::Program arithmetic_program() {
    suffice::Program program;
    const auto a = program.load(0);
    const auto b = program.load(1);
    const auto difference = program.sub(a, b);
    const auto linear = program.sub(program.mul_const(a, 3), program.mul_const(b, 2));
    program.output(program.add(program.abs(linear), program.square(difference)), 15);
    return program;
}

i64 reference(i64 a, i64 b) {
    const i64 linear = 3 * a - 2 * b;
    const i64 difference = a - b;
    return (linear < 0 ? -linear : linear) + difference * difference;
}

void exact_and_bounds_exhaustive() {
    const std::array<Bounds, 2> ranges{{{-8, 8}, {-8, 8}}};
    const auto program = arithmetic_program().compile(ranges);
    for (i64 a = -8; a <= 8; ++a) {
        for (i64 b = -8; b <= 8; ++b) {
            const std::array<i64, 2> exact{{a, b}};
            const std::array<Bounds, 2> precise{{{a, a}, {b, b}}};
            const auto value = reference(a, b);
            require(program.evaluate_exact(exact) == value, "exact arithmetic differs");
            require(program.evaluate_exact_unchecked(exact) == value, "fast exact arithmetic differs");
            require(program.evaluate_bounds(precise) == Bounds{value, value}, "point interval differs");
            require(program.decide(precise) == (value > 15 ? Decision::True : Decision::False),
                    "exact decision differs");
        }
    }
    for (i64 al = -3; al <= 3; ++al) {
        for (i64 ah = al; ah <= 3; ++ah) {
            for (i64 bl = -3; bl <= 3; ++bl) {
                for (i64 bh = bl; bh <= 3; ++bh) {
                    const std::array<Bounds, 2> box{{{al, ah}, {bl, bh}}};
                    const auto interval = program.evaluate_bounds(box);
                    const auto decision = program.decide(interval);
                    for (i64 a = al; a <= ah; ++a) {
                        for (i64 b = bl; b <= bh; ++b) {
                            const auto value = reference(a, b);
                            require(interval.lo <= value && value <= interval.hi,
                                    "interval excludes an exact result");
                            if (decision == Decision::True) require(value > 15, "unsound true decision");
                            if (decision == Decision::False) require(value <= 15, "unsound false decision");
                        }
                    }
                }
            }
        }
    }
}

void random_interval_soundness() {
    std::mt19937_64 random(0x53554646494345ULL);
    const std::array<Bounds, 2> ranges{{{-100000, 100000}, {-100000, 100000}}};
    const auto program = arithmetic_program().compile(ranges);
    for (int trial = 0; trial < 10000; ++trial) {
        const i64 a = static_cast<i64>(random() % 200001) - 100000;
        const i64 b = static_cast<i64>(random() % 200001) - 100000;
        const i64 aleft = static_cast<i64>(random() % 1024);
        const i64 aright = static_cast<i64>(random() % 1024);
        const i64 bleft = static_cast<i64>(random() % 1024);
        const i64 bright = static_cast<i64>(random() % 1024);
        const std::array<Bounds, 2> coarse{{{a - aleft, a + aright}, {b - bleft, b + bright}}};
        const auto result = program.evaluate_bounds(coarse);
        const auto value = reference(a, b);
        require(result.lo <= value && value <= result.hi, "random bounds unsound");
    }
}

void optimizer_and_pruning() {
    suffice::Program p;
    const auto a = p.load(0);
    const auto b = p.load(1, -1);
    require(p.load(1, -1) == b, "identical loads not interned");
    require(p.add(a, b) == p.add(b, a), "commutative CSE missing");
    require(p.min(a, b) == p.min(b, a), "min CSE missing");
    require(p.max(a, b) == p.max(b, a), "max CSE missing");
    const auto zero = p.constant(0);
    require(p.add(a, zero) == a && p.sub(a, zero) == a, "zero identities missing");
    require(p.mul_const(a, 1) == a && p.mul_const(a, 0) == zero, "scale identities missing");
    require(p.sub(a, a) == zero, "correlated subtraction not simplified");
    const auto magnitude = p.abs(a);
    require(p.abs(magnitude) == magnitude, "nested abs not simplified");
    const auto square = p.square(a);
    require(p.square(magnitude) == square && p.abs(square) == square, "square identities missing");

    // Even invalid-channel loads and more than 64 source nodes can be removed
    // when they cannot affect the output.
    for (std::uint32_t c = 100; c < 200; ++c) (void)p.load(c);
    p.output(zero, -1);
    const auto compiled = p.compile({});
    require(compiled.node_count() == 1 && compiled.loads().empty(), "dead nodes survived");
    require(compiled.evaluate_exact({}) == 0, "constant program incorrect");
    require(compiled.decide(std::span<const Bounds>{}) == Decision::True, "constant decision incorrect");

    suffice::Program too_large;
    auto value = too_large.load(0);
    const auto one = too_large.constant(1);
    for (int i = 0; i < 70; ++i) value = too_large.add(value, one);
    too_large.output(value, 0);
    const std::array<Bounds, 1> range{{{-10, 10}}};
    require_throws<std::length_error>([&] { (void)too_large.compile(range); }, "64-node limit not enforced");
}

void overflow_and_range_contracts() {
    const i64 low = std::numeric_limits<i64>::min();
    const i64 high = std::numeric_limits<i64>::max();
    const std::array<Bounds, 1> full{{{std::numeric_limits<std::int32_t>::min(),
                                      std::numeric_limits<std::int32_t>::max()}}};
    auto energy = suffice::builtin_program("energy", 0).compile(full);
    const std::array<i64, 1> lowest_input{{std::numeric_limits<std::int32_t>::min()}};
    require(energy.evaluate_exact(lowest_input) == 4611686018427387904LL,
            "int32-min squared should fit int64");

    require_throws<std::overflow_error>([&] {
        suffice::Program p;
        const auto x = p.load(0);
        p.output(p.square(p.mul_const(x, 2)), 0);
        (void)p.compile(full);
    }, "range analysis allowed square overflow");
    require_throws<std::overflow_error>([&] {
        suffice::Program p;
        const auto c = p.constant(low);
        (void)p.abs(c);
    }, "abs(INT64_MIN) accepted");
    require_throws<std::overflow_error>([&] {
        suffice::Program p;
        const auto c = p.constant(low);
        (void)p.mul_const(c, -1);
    }, "INT64_MIN * -1 accepted");
    require_throws<std::overflow_error>([&] {
        suffice::Program p;
        const auto a = p.constant(high), b = p.constant(1);
        (void)p.add(a, b);
    }, "constant addition overflow accepted");
    require_throws<std::overflow_error>([&] {
        suffice::Program p;
        const auto a = p.constant(low), b = p.constant(1);
        (void)p.sub(a, b);
    }, "constant subtraction overflow accepted");

    // Cover all multiplication sign branches and the one asymmetric limit.
    const std::array<i64, 7> values{{low, -high, -2, -1, 0, 1, high}};
    for (const auto value : values) {
        suffice::Program p;
        const auto c = p.constant(value);
        p.output(p.mul_const(c, 1), 0);
        require(p.compile({}).evaluate_exact({}) == value, "multiply by one changed value");
        p.output(p.mul_const(c, 0), 0);
        require(p.compile({}).evaluate_exact({}) == 0, "multiply by zero changed value");
        if (value != low) {
            p.output(p.mul_const(c, -1), 0);
            require(p.compile({}).evaluate_exact({}) == -value, "negation changed value");
        }
    }
    const std::array<std::array<i64, 2>, 5> multiplication_overflows{{
        {{high, 2}}, {{-high, 2}}, {{high, -2}}, {{-high, -2}}, {{-(i64{1} << 62), -2}}
    }};
    for (const auto pair : multiplication_overflows) {
        require_throws<std::overflow_error>([&] {
            suffice::Program p;
            (void)p.mul_const(p.constant(pair[0]), pair[1]);
        }, "multiplication sign branch allowed overflow");
    }
    for (const auto pair : std::array<std::array<i64, 2>, 2>{{
             {{-(i64{1} << 62), 2}}, {{i64{1} << 62, -2}}}}) {
        suffice::Program p;
        p.output(p.mul_const(p.constant(pair[0]), pair[1]), 0);
        require(p.compile({}).evaluate_exact({}) == low, "valid INT64_MIN product rejected");
    }
    require_throws<std::invalid_argument>([&] {
        (void)suffice::builtin_program("identity", 0).compile(std::array<Bounds, 1>{{{2, 1}}});
    }, "invalid compile range accepted");
    require_throws<std::invalid_argument>([&] {
        (void)suffice::builtin_program("identity", 0).compile(std::array<Bounds, 1>{{{low, high}}});
    }, "non-int32 input range accepted");
    const std::array<Bounds, 1> small{{{-10, 10}}};
    const auto identity = suffice::builtin_program("identity", 0).compile(small);
    require(identity.evaluate_bounds(std::array<Bounds, 1>{{{-100, 100}}}) == Bounds{-10, 10},
            "coarse intervals were not intersected with compile range");
    require_throws<std::out_of_range>([&] {
        (void)identity.evaluate_exact(std::array<i64, 1>{{11}});
    }, "out-of-range exact input accepted");
    require_throws<std::out_of_range>([&] {
        (void)identity.evaluate_bounds(std::array<Bounds, 1>{{{11, 20}}});
    }, "disjoint input interval accepted");
    require_throws<std::invalid_argument>([&] { (void)identity.evaluate_exact({}); },
                                          "missing input accepted");
    require(identity.decide(Bounds{0, 0}) == Decision::False, "strict threshold tie incorrect");
    require(identity.decide(Bounds{1, 1}) == Decision::True, "strict threshold true incorrect");
    require(identity.decide(Bounds{-1, 1}) == Decision::Unknown, "ambiguity not detected");
}

void parser_and_builtins() {
    const auto parsed = suffice::parse_program(
        "# Three sample filter\n"
        "input center 0 0; input left 0 -1; input right 0 1\n"
        "scale twice center +2\n"
        "sub a left twice\nadd b a right\nabs magnitude b\n"
        "output magnitude > 4\n");
    const std::array<Bounds, 1> range{{{-100, 100}}};
    const auto program = parsed.compile(range);
    require(program.min_offset() == -1 && program.max_offset() == 1, "halo offsets wrong");
    require(program.loads().size() == 3 && program.loads()[0].offset == 0 &&
            program.loads()[1].offset == -1 && program.loads()[2].offset == 1,
            "stable load ordering changed");
    const std::array<i64, 3> inputs{{10, 7, 17}};
    require(program.evaluate_exact(inputs) == 4, "parsed FIR differs");
    require(program.decide(Bounds{4, 4}) == Decision::False, "threshold tie wrong");
    const auto builtin = suffice::builtin_program("fir3", 4).compile(range);
    require(builtin.evaluate_exact(inputs) == 4, "builtin FIR differs");
    require(!program.describe().empty(), "plan description empty");

    const std::array<const char*, 9> invalid{{
        "input a 0 0\n", "input a 0 0\noutput a >= 0", "input a 0 17\noutput a > 0",
        "input a -1 0\noutput a > 0", "input a 0 0\ninput a 0 1\noutput a > 0",
        "input a 0 0\noutput b > 0", "const a 9223372036854775808\noutput a > 0",
        "const a 0\noutput a > 0\noutput a > 0", "input a 0 0\nnonsense b a\noutput b > 0"
    }};
    for (auto text : invalid)
        require_throws<std::invalid_argument>([&] { (void)suffice::parse_program(text); },
                                             "malformed DSL accepted");

    const auto minmax = suffice::parse_program(
        "input a 0 0; input b 1 0; min lower a b; max upper a b; sub span upper lower; output span > 3");
    const std::array<Bounds, 2> ranges{{{-5, 5}, {-5, 5}}};
    const auto extrema = minmax.compile(ranges);
    require(extrema.evaluate_exact(std::array<i64, 2>{{-4, 3}}) == 7, "min/max incorrect");
}

} // namespace

void test_ir() {
    exact_and_bounds_exhaustive();
    random_interval_soundness();
    optimizer_and_pruning();
    overflow_and_range_contracts();
    parser_and_builtins();
}
