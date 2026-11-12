#include "suffice/frontier.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using suffice::FrontierPlan;
using suffice::PredicateNodeId;
using suffice::PredicateOp;
using suffice::PredicatePlan;
using suffice::WordcodeNode;
using suffice::WordcodeOp;
constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
constexpr auto maximum = std::numeric_limits<std::int32_t>::max();

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("frontier test: ") + reason);
}
template<class Function> void rejects(Function&& function, const char* reason) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, reason);
}

std::uint64_t random_bits(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

PredicatePlan one_comparison(std::int32_t threshold, suffice::Bounds range = {minimum, maximum}) {
    PredicatePlan source;
    source.complete = true;
    source.node_count = source.load_count = source.certificate_count = 1;
    source.nodes[0] = {PredicateOp::Greater, 0, 0, 0, threshold};
    source.loads[0] = source.certificates[0] = {0, 0, range};
    return source;
}

PredicateNodeId add(PredicatePlan& source, PredicateOp op,
                    PredicateNodeId a = 0, PredicateNodeId b = 0,
                    std::uint16_t load = 0, std::int32_t threshold = 0) {
    require(source.node_count < suffice::max_predicate_nodes, "test predicate capacity exhausted");
    const auto id = source.node_count++;
    source.nodes[id] = {op, a, b, load, threshold};
    source.root = id;
    return id;
}

std::uint64_t active_mask(std::size_t rows) {
    require(rows <= 64, "test mask exceeds one word");
    return rows == 64 ? UINT64_MAX : ((std::uint64_t{1} << rows) - 1);
}

bool truth(const PredicatePlan& source, std::span<const std::int32_t> row) {
    std::array<bool, suffice::max_predicate_nodes> values{};
    for (std::size_t i = 0; i < source.node_count; ++i) {
        const auto& node = source.nodes[i];
        switch (node.op) {
        case PredicateOp::False: values[i] = false; break;
        case PredicateOp::True: values[i] = true; break;
        case PredicateOp::Greater:
            values[i] = row[source.loads[node.load_index].channel] > node.threshold; break;
        case PredicateOp::And: values[i] = values[node.a] && values[node.b]; break;
        case PredicateOp::Or: values[i] = values[node.a] || values[node.b]; break;
        case PredicateOp::Not: values[i] = !values[node.a]; break;
        }
    }
    return values[source.root];
}

std::uint64_t reference(const PredicatePlan& source, std::span<const std::int32_t> input,
                        std::size_t channels) {
    require(channels && input.size() % channels == 0, "invalid test rows");
    const auto rows = input.size() / channels;
    std::uint64_t result = 0;
    for (std::size_t row = 0; row < rows; ++row)
        if (truth(source, input.subspan(row * channels, channels))) result |= std::uint64_t{1} << row;
    return result;
}

std::vector<std::uint64_t> forest(std::span<const WordcodeNode> nodes,
                                 std::span<const suffice::Load> loads,
                                 std::span<const std::int32_t> input, std::size_t channels) {
    const auto rows = input.size() / channels;
    std::vector<std::uint64_t> values(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        switch (node.op) {
        case WordcodeOp::False: values[i] = 0; break;
        case WordcodeOp::True: values[i] = UINT64_MAX; break;
        case WordcodeOp::Plane: {
            const auto channel = loads[node.load_index].channel;
            for (std::size_t row = 0; row < rows; ++row) {
                const auto biased = std::bit_cast<std::uint32_t>(input[row * channels + channel]) ^ 0x80000000U;
                values[i] |= std::uint64_t((biased >> (31U - node.bit)) & 1U) << row;
            }
            break;
        }
        case WordcodeOp::And: values[i] = values[node.a] & values[node.b]; break;
        case WordcodeOp::Or: values[i] = values[node.a] | values[node.b]; break;
        case WordcodeOp::Not: values[i] = ~values[node.a]; break;
        }
    }
    return values;
}

struct BoundsMasks { std::uint64_t lower{}, upper{}; };

BoundsMasks check(const PredicatePlan& source, const FrontierPlan& plan,
                  std::span<const std::int32_t> input, std::size_t channels,
                  bool check_arena = false) {
    require(plan.complete && plan.exact.complete, "valid source did not compile completely");
    const auto rows = input.size() / channels;
    const auto valid = active_mask(rows);
    const auto original = reference(source, input, channels);
    const auto prefix = forest(plan.prefix.nodes, plan.prefix.loads, input, channels);
    const auto exact = forest(plan.exact.nodes, plan.exact.loads, input, channels);
    const auto lower = prefix[plan.prefix.must_root] & valid;
    const auto upper = prefix[plan.prefix.may_root] & valid;
    require((lower & ~original) == 0, "must mask contains a false row");
    require((original & ~upper) == 0, "may mask excludes a true row");
    require((lower & ~upper) == 0, "Boolean interval is inverted");
    require((exact[plan.exact.root] & valid) == original, "exact fallback differs from predicate oracle");
    const auto unknown = upper ^ lower;
    const auto composed = lower | (unknown & exact[plan.exact.root]);
    require(composed == original, "prefix and exact fallback do not reconstruct the result");
    if (plan.prefix_bits == 32) require(lower == original && upper == original, "32-bit prefix remains inexact");
    for (const auto& node : plan.prefix.nodes)
        if (node.op == WordcodeOp::Plane) require(node.bit < plan.prefix_bits, "prefix reads an unknown bit");

    if (check_arena) {
        std::vector<std::uint64_t> arena(plan.prefix.live_slot_count, 0xdeadbeefbeefdeadULL);
        const auto get = [&](std::uint32_t id) {
            const auto slot = plan.prefix.nodes[id].slot;
            return slot == suffice::no_wordcode_slot ? prefix[id] : arena[slot];
        };
        for (const auto& node : plan.prefix.nodes) {
            if (node.slot == suffice::no_wordcode_slot) {
                require(node.op == WordcodeOp::False || node.op == WordcodeOp::True ||
                        node.op == WordcodeOp::Plane, "operation has no arena slot");
                continue;
            }
            require(node.slot < arena.size(), "arena slot is out of range");
            const auto a = get(node.a);
            const auto b = node.op == WordcodeOp::Not ? 0 : get(node.b);
            switch (node.op) {
            case WordcodeOp::And: arena[node.slot] = a & b; break;
            case WordcodeOp::Or: arena[node.slot] = a | b; break;
            case WordcodeOp::Not: arena[node.slot] = ~a; break;
            default: throw std::runtime_error("frontier test: leaf owns an arena slot");
            }
        }
        require((get(plan.prefix.must_root) & valid) == lower, "must root was overwritten by slot reuse");
        require((get(plan.prefix.may_root) & valid) == upper, "may root was overwritten by slot reuse");
    }
    return {lower, upper};
}

void atomic_bounds() {
    std::vector<std::int32_t> thresholds{
        minimum, minimum + 1, minimum + 2, minimum + 127, -16777217, -65537,
        -4097, -257, -129, -2, -1, 0, 1, 2, 7, 127, 128, 255, 256, 999,
        1000, 4095, 65535, 16777215, maximum - 1, maximum};
    std::uint64_t random = 0x46524f4e54494552ULL;
    for (unsigned i = 0; i < 48; ++i)
        thresholds.push_back(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random_bits(random))));
    for (const auto threshold : thresholds) {
        const auto source = one_comparison(threshold);
        std::array<std::int32_t, 64> values{};
        values[0] = minimum; values[1] = maximum; values[2] = threshold;
        values[3] = threshold == minimum ? minimum : threshold - 1;
        values[4] = threshold == maximum ? maximum : threshold + 1;
        for (std::size_t i = 5; i < values.size(); ++i)
            values[i] = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random_bits(random)));
        BoundsMasks previous{0, UINT64_MAX};
        for (unsigned bits = 0; bits <= 32; ++bits) {
            const auto plan = suffice::compile_frontier(source, bits);
            const auto bounds = check(source, plan, values, 1, true);
            require((previous.lower & ~bounds.lower) == 0, "atomic lower bound weakened under refinement");
            require((bounds.upper & ~previous.upper) == 0, "atomic upper bound weakened under refinement");
            previous = bounds;
            const auto width = std::uint64_t{1} << (32U - bits);
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto ordered = std::uint64_t(std::bit_cast<std::uint32_t>(values[i]) ^ 0x80000000U);
                // Independent oracle: enumerate the enclosing numeric interval
                // algebraically, not by rounded comparison thresholds.
                const auto low = (ordered / width) * width;
                const auto high = low + width - 1;
                const auto signed_low = static_cast<std::int64_t>(low) - (std::int64_t{1} << 31);
                const auto signed_high = static_cast<std::int64_t>(high) - (std::int64_t{1} << 31);
                require(bool((bounds.lower >> i) & 1U) == (signed_low > threshold), "atomic must differs from completion minimum");
                require(bool((bounds.upper >> i) & 1U) == (signed_high > threshold), "atomic may differs from completion maximum");
            }
        }
    }
}

void exhaustive_small_domain() {
    const std::array<unsigned, 14> depths{0, 1, 12, 16, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    std::array<std::int32_t, 256> values{};
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<std::int32_t>(i) - 128;
    for (std::int32_t threshold = -129; threshold <= 127; ++threshold) {
        const auto source = one_comparison(threshold, {-128, 127});
        for (const auto depth : depths) {
            const auto plan = suffice::compile_frontier(source, depth);
            for (std::size_t first = 0; first < values.size(); first += 64) {
                const auto bounds = check(source, plan, std::span(values).subspan(first, 64), 1);
                const auto width = std::uint64_t{1} << (32U - depth);
                for (std::size_t local = 0; local < 64; ++local) {
                    const auto value = values[first + local];
                    const auto ordered = std::uint64_t(std::bit_cast<std::uint32_t>(value) ^ 0x80000000U);
                    const auto low = static_cast<std::int64_t>((ordered / width) * width) - (std::int64_t{1} << 31);
                    const auto high = low + static_cast<std::int64_t>(width) - 1;
                    const auto admitted_low = std::max<std::int64_t>(low, -128);
                    const auto admitted_high = std::min<std::int64_t>(high, 127);
                    require(bool((bounds.lower >> local) & 1U) == (admitted_low > threshold), "certified atomic minimum differs");
                    require(bool((bounds.upper >> local) & 1U) == (admitted_high > threshold), "certified atomic maximum differs");
                }
            }
        }
    }
}

PredicatePlan compound(std::int32_t t0, std::int32_t t1, std::int32_t t2,
                       suffice::Bounds range = {minimum, maximum}) {
    PredicatePlan source;
    source.complete = true;
    source.load_count = source.certificate_count = 3;
    for (std::uint16_t i = 0; i < 3; ++i) source.loads[i] = source.certificates[i] = {i, 0, range};
    const auto a = add(source, PredicateOp::Greater, 0, 0, 0, t0);
    const auto b = add(source, PredicateOp::Greater, 0, 0, 1, t1);
    const auto c = add(source, PredicateOp::Greater, 0, 0, 2, t2);
    const auto left = add(source, PredicateOp::Or, a, b);
    const auto not_a = add(source, PredicateOp::Not, a);
    const auto right = add(source, PredicateOp::Or, not_a, c);
    add(source, PredicateOp::And, left, right);
    return source;
}

void compound_bounds_and_correlation() {
    auto source = compound(0, -1, -1);
    const auto partial = suffice::compile_frontier(source, 1);
    const std::array<std::int32_t, 6> correlated{0, -1, -1, 1, -1, -1};
    const auto bounds = check(source, partial, correlated, 3, true);
    require(bounds.lower == 0 && bounds.upper == 3, "correlation example no longer demonstrates conservative bounds");

    std::uint64_t random = 0x626f756e64732d34ULL;
    const std::array<std::int32_t, 7> thresholds{-1001, -1, 0, 1, 1000, minimum, maximum};
    for (const auto t : thresholds) {
        source = compound(t, t == minimum ? minimum : t - 1, t == maximum ? maximum : t + 1);
        std::array<std::int32_t, 64 * 3> input{};
        for (auto& value : input)
            value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random_bits(random)));
        for (std::size_t i = 0; i < 12; ++i) input[i] = t;
        BoundsMasks previous{0, UINT64_MAX};
        for (unsigned bits = 0; bits <= 32; ++bits) {
            const auto plan = suffice::compile_frontier(source, bits);
            const auto current = check(source, plan, input, 3, true);
            require((previous.lower & ~current.lower) == 0 && (current.upper & ~previous.upper) == 0,
                    "compound refinement increased uncertainty");
            previous = current;
        }
    }

    // Every possible input in a three-channel four-bit domain, for every
    // prefix depth. This exercises AND, OR, NOT and their correlations.
    source = compound(-1, 2, -4, {-8, 7});
    for (unsigned bits = 0; bits <= 32; ++bits) {
        const auto plan = suffice::compile_frontier(source, bits);
        std::vector<std::int32_t> batch;
        batch.reserve(64 * 3);
        for (std::int32_t a = -8; a <= 7; ++a)
            for (std::int32_t b = -8; b <= 7; ++b)
                for (std::int32_t c = -8; c <= 7; ++c) {
                    batch.insert(batch.end(), {a, b, c});
                    if (batch.size() == 64 * 3) { (void)check(source, plan, batch, 3, true); batch.clear(); }
                }
        require(batch.empty(), "exhaustive batch remainder is unexpected");
    }

    // Distinct source load indices can refer to one physical channel with
    // intersecting contracts. Canonicalization must use that intersection.
    source = compound(12, 20, 25, {0, 100});
    source.loads[1] = source.certificates[1] = {0, 0, {10, 90}};
    source.loads[2] = source.certificates[2] = {0, 0, {20, 80}};
    const auto aliases = suffice::compile_frontier(source, 28);
    require(aliases.prefix.loads.size() == 1 && aliases.exact.loads.size() == 1,
            "same physical channel was not canonicalized");
    require(aliases.prefix.loads[0].range == suffice::Bounds{20, 80}, "range intersections were not preserved");
    std::array<std::int32_t, 61> alias_values{};
    for (std::size_t i = 0; i < alias_values.size(); ++i) alias_values[i] = static_cast<std::int32_t>(i) + 20;
    (void)check(source, aliases, alias_values, 1, true);
}

void random_dags() {
    std::uint64_t random = 0x636f6d70696c6572ULL;
    for (unsigned trial = 0; trial < 32; ++trial) {
        PredicatePlan source;
        source.complete = true;
        source.load_count = source.certificate_count = 4;
        for (std::uint16_t i = 0; i < 4; ++i)
            source.loads[i] = source.certificates[i] = {i, 0, {minimum, maximum}};
        for (unsigned i = 0; i < 12; ++i)
            add(source, PredicateOp::Greater, 0, 0, static_cast<std::uint16_t>(i % 4),
                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random_bits(random))));
        for (unsigned i = 12; i < 90; ++i) {
            const auto a = static_cast<PredicateNodeId>(random_bits(random) % i);
            const auto b = static_cast<PredicateNodeId>(random_bits(random) % i);
            const auto choose = random_bits(random) % 3;
            add(source, choose == 0 ? PredicateOp::And : choose == 1 ? PredicateOp::Or : PredicateOp::Not, a, b);
        }
        std::array<std::int32_t, 64 * 4> input{};
        for (auto& value : input)
            value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random_bits(random)));
        BoundsMasks previous{0, UINT64_MAX};
        for (const unsigned bits : {0U, 1U, 4U, 8U, 12U, 16U, 24U, 31U, 32U}) {
            const auto plan = suffice::compile_frontier(source, bits);
            const auto current = check(source, plan, input, 4, true);
            require((previous.lower & ~current.lower) == 0 && (current.upper & ~previous.upper) == 0,
                    "random DAG refinement weakened the result");
            previous = current;
            suffice::validate_frontier(plan);
        }
    }
}

void certificates_budgets_and_validation() {
    auto source = one_comparison(100, {0, 10});
    source.certificate_count = 3;
    source.certificates[1] = {7, -16, {-5, 12}};
    source.certificates[2] = {7, 16, {0, 9}};
    const auto folded = suffice::compile_frontier(source, 0);
    require(folded.complete && folded.prefix.plane_count == 0, "constant proof did not remove reads");
    require(folded.prefix.loads.size() == 2 && folded.exact.loads.size() == 2,
            "proof-only source channel was discarded");
    require(folded.prefix.loads[0].channel == 0 && folded.prefix.loads[0].range == suffice::Bounds{0, 10},
            "folding certificate was lost");
    require(folded.prefix.loads[1].channel == 7 && folded.prefix.loads[1].offset == 0 &&
            folded.prefix.loads[1].range == suffice::Bounds{0, 9}, "halo certificate intersection was lost");
    suffice::validate_frontier(folded);
    const auto at_budget = suffice::compile_frontier(source, 0, 1);
    require(at_budget.complete, "one-node certified constant exceeds a one-node budget");

    const auto failure_is_empty = [](const FrontierPlan& plan) {
        require(!plan.complete && !plan.reason.empty(), "failed compilation lacks its reason");
        require(plan.prefix.nodes.empty() && plan.prefix.loads.empty() &&
                plan.exact.nodes.empty() && plan.exact.loads.empty() && !plan.exact.complete,
                "failed compilation leaked a partial executable plan");
        rejects([&] { suffice::validate_frontier(plan); }, "failed frontier was validated");
    };
    source = one_comparison(0x12345678);
    failure_is_empty(suffice::compile_frontier(source, 12, 1));
    for (const auto budget : {2U, 3U, 8U, 16U}) failure_is_empty(suffice::compile_frontier(source, 12, budget));
    rejects([&] { (void)suffice::compile_frontier(source, 33); }, "invalid prefix depth accepted");
    rejects([&] { (void)suffice::compile_frontier(source, 12, 0); }, "zero budget accepted");
    rejects([&] { (void)suffice::compile_frontier(source, 12, suffice::max_wordcode_nodes + 1); }, "unbounded budget accepted");
    auto malformed = source;
    malformed.complete = false;
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.nodes[0].load_index = 1;
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.nodes[0].op = static_cast<PredicateOp>(255);
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.loads[0].offset = 1;
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.certificate_count = 65;
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.certificates[0].offset = 17;
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.certificates[0].range = {maximum, minimum};
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = source; malformed.nodes[0] = {PredicateOp::Not, 0};
    failure_is_empty(suffice::compile_frontier(malformed));
    malformed = one_comparison(5, {0, 10});
    malformed.certificate_count = 2; malformed.certificates[1] = {0, 0, {20, 30}};
    failure_is_empty(suffice::compile_frontier(malformed));

    const auto valid = suffice::compile_frontier(source, 12);
    require(valid.complete, "validation fixture incomplete");
    suffice::validate_frontier(valid);
    auto changed = valid;
    std::swap(changed.prefix.must_root, changed.prefix.may_root);
    rejects([&] { suffice::validate_frontier(changed); }, "swapped bounds accepted");
    changed = valid; changed.prefix.loads[0].range.lo += 1;
    rejects([&] { suffice::validate_frontier(changed); }, "modified prefix certificate accepted");
    changed = valid; changed.exact.loads[0].range.hi -= 1;
    rejects([&] { suffice::validate_frontier(changed); }, "modified exact certificate accepted");
    changed = valid; changed.prefix.live_slot_count += 1;
    rejects([&] { suffice::validate_frontier(changed); }, "modified liveness accepted");
    changed = valid; changed.exact.nodes[changed.exact.root].op = WordcodeOp::Not;
    rejects([&] { suffice::validate_frontier(changed); }, "modified exact fallback accepted");
    changed = valid; changed.prefix_bits = 33;
    rejects([&] { suffice::validate_frontier(changed); }, "modified unbounded depth accepted");
    changed = valid; changed.prefix.nodes.clear();
    rejects([&] { suffice::validate_frontier(changed); }, "empty executable prefix accepted");
    changed = folded; changed.prefix.loads.pop_back();
    rejects([&] { suffice::validate_frontier(changed); }, "deleted proof-only certificate accepted");
    FrontierPlan fabricated;
    fabricated.complete = true;
    fabricated.prefix = valid.prefix;
    fabricated.exact = valid.exact;
    rejects([&] { suffice::validate_frontier(fabricated); }, "public graph without construction witness accepted");
    auto copied = valid;
    auto moved = std::move(copied);
    suffice::validate_frontier(moved);
    suffice::validate_frontier(valid);
    require(valid.describe().find("prefix_bits=12") != std::string::npos, "description omits prefix depth");
}

} // namespace

void test_frontier() {
    atomic_bounds();
    exhaustive_small_domain();
    compound_bounds_and_correlation();
    random_dags();
    certificates_budgets_and_validation();
}
