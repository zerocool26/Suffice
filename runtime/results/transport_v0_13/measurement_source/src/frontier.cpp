#include "suffice/frontier.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace suffice {
namespace {

struct Failure { std::string reason; };

bool operation(WordcodeOp op) noexcept {
    return op == WordcodeOp::And || op == WordcodeOp::Or || op == WordcodeOp::Not;
}

struct Key {
    WordcodeOp op{};
    std::uint32_t a{}, b{};
    std::uint16_t load{};
    std::uint8_t bit{};
    friend bool operator==(const Key&, const Key&) = default;
};
struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        auto value = std::uint64_t(key.a) * 0x9e3779b185ebca87ULL;
        value ^= std::uint64_t(key.b) * 0xc2b2ae3d27d4eb4fULL;
        value ^= std::uint64_t(key.load) * 0x165667b19e3779f9ULL;
        value ^= (std::uint64_t(key.bit) << 8) | static_cast<unsigned>(key.op);
        value ^= value >> 33;
        return static_cast<std::size_t>(value);
    }
};
struct AtomKey {
    std::uint16_t load{};
    std::int32_t threshold{};
    friend bool operator==(const AtomKey&, const AtomKey&) = default;
};
struct AtomHash {
    std::size_t operator()(const AtomKey& key) const noexcept {
        const auto threshold = std::bit_cast<std::uint32_t>(key.threshold);
        return static_cast<std::size_t>((std::uint64_t(key.load) << 32) | threshold);
    }
};

class StageCompiler {
public:
    StageCompiler(const PredicatePlan& source, const WordcodePlan& exact,
                  unsigned prefix_bits, std::uint32_t budget)
        : source_(source), exact_(exact), prefix_bits_(prefix_bits), budget_(budget) {
        result_.loads = exact.loads;
        for (std::uint16_t i = 0; i < source.load_count; ++i)
            load_map_[i] = physical_load(source.loads[i].channel);
    }

    FrontierStage compile() {
        const auto exact_op = exact_.nodes[exact_.root].op;
        if (exact_op == WordcodeOp::False || exact_op == WordcodeOp::True) {
            result_.must_root = result_.may_root = constant(exact_op == WordcodeOp::True);
            finish();
            return std::move(result_);
        }

        std::array<std::uint32_t, max_predicate_nodes> lower{}, upper{};
        for (std::size_t i = 0; i < source_.node_count; ++i) {
            const auto& node = source_.nodes[i];
            switch (node.op) {
            case PredicateOp::False:
            case PredicateOp::True:
                lower[i] = upper[i] = constant(node.op == PredicateOp::True);
                break;
            case PredicateOp::Greater: {
                const auto load = load_map_[node.load_index];
                const auto biased = std::bit_cast<std::uint32_t>(node.threshold) ^ 0x80000000U;
                const auto unknown = 32U - prefix_bits_;
                // Explicit endpoints avoid shifting a 32-bit value by 32.
                const auto suffix = unknown == 32 ? UINT32_MAX :
                    (unknown ? (std::uint32_t{1} << unknown) - 1U : 0U);
                lower[i] = greater(load, unbias(biased | suffix));
                if ((biased & suffix) == suffix) {
                    upper[i] = lower[i];
                } else {
                    const auto start = biased & ~suffix;
                    upper[i] = start == 0 ? constant(true) : greater(load, unbias(start - 1U));
                }
                break;
            }
            case PredicateOp::And:
            case PredicateOp::Or: {
                const auto op = node.op == PredicateOp::And ? WordcodeOp::And : WordcodeOp::Or;
                lower[i] = binary(op, lower[node.a], lower[node.b]);
                upper[i] = binary(op, upper[node.a], upper[node.b]);
                break;
            }
            case PredicateOp::Not:
                lower[i] = negate(upper[node.a]);
                upper[i] = negate(lower[node.a]);
                break;
            }
        }
        result_.must_root = lower[source_.root];
        result_.may_root = upper[source_.root];
        finish();
        return std::move(result_);
    }

private:
    struct Atom {
        bool known{}, inverted{};
        std::uint16_t load{};
        std::int32_t threshold{};
    };
    const PredicatePlan& source_;
    const WordcodePlan& exact_;
    unsigned prefix_bits_{};
    std::uint32_t budget_{};
    FrontierStage result_;
    std::array<std::uint16_t, max_program_nodes> load_map_{};
    std::unordered_map<Key, std::uint32_t, KeyHash> interned_;
    std::unordered_map<AtomKey, std::uint32_t, AtomHash> comparisons_;
    std::vector<Atom> atoms_;

    static std::int32_t unbias(std::uint32_t value) noexcept {
        return std::bit_cast<std::int32_t>(value ^ 0x80000000U);
    }

    std::uint16_t physical_load(std::uint32_t channel) const {
        const auto it = std::find_if(result_.loads.begin(), result_.loads.end(),
            [channel](const Load& load) { return load.channel == channel; });
        if (it == result_.loads.end()) throw Failure{"frontier lost a source channel certificate"};
        return static_cast<std::uint16_t>(it - result_.loads.begin());
    }

    std::uint32_t intern(Key key) {
        if (const auto it = interned_.find(key); it != interned_.end()) return it->second;
        if (result_.nodes.size() >= budget_)
            throw Failure{"frontier prefix node budget exhausted; no partial plan is executable"};
        const auto id = static_cast<std::uint32_t>(result_.nodes.size());
        result_.nodes.push_back({key.op, key.a, key.b, key.load, key.bit, no_wordcode_slot});
        atoms_.push_back({});
        interned_.emplace(key, id);
        return id;
    }

    std::uint32_t constant(bool value) {
        return intern({value ? WordcodeOp::True : WordcodeOp::False});
    }

    bool is_constant(std::uint32_t id, bool value) const noexcept {
        return result_.nodes[id].op == (value ? WordcodeOp::True : WordcodeOp::False);
    }

    bool complements(std::uint32_t a, std::uint32_t b) const noexcept {
        return (result_.nodes[a].op == WordcodeOp::Not && result_.nodes[a].a == b) ||
               (result_.nodes[b].op == WordcodeOp::Not && result_.nodes[b].a == a);
    }

    bool contains(std::uint32_t id, WordcodeOp op, std::uint32_t child) const noexcept {
        const auto& node = result_.nodes[id];
        return node.op == op && (node.a == child || node.b == child);
    }

    std::uint32_t binary(WordcodeOp op, std::uint32_t a, std::uint32_t b) {
        if (a == b) return a;
        const bool is_and = op == WordcodeOp::And;
        if (is_constant(a, !is_and)) return a;
        if (is_constant(b, !is_and)) return b;
        if (is_constant(a, is_and)) return b;
        if (is_constant(b, is_and)) return a;
        if (complements(a, b)) return constant(!is_and);
        const auto left = atoms_[a], right = atoms_[b];
        if (left.known && right.known && left.load == right.load) {
            if (left.inverted == right.inverted) {
                const bool want_larger = is_and != left.inverted;
                return (left.threshold >= right.threshold) == want_larger ? a : b;
            }
            const auto greater_than = left.inverted ? right.threshold : left.threshold;
            const auto at_most = left.inverted ? left.threshold : right.threshold;
            if (is_and && greater_than >= at_most) return constant(false);
            if (!is_and && greater_than <= at_most) return constant(true);
        }
        const auto other = is_and ? WordcodeOp::Or : WordcodeOp::And;
        if (contains(a, other, b)) return b;
        if (contains(b, other, a)) return a;
        if (a > b) std::swap(a, b);
        return intern({op, a, b});
    }

    std::uint32_t negate(std::uint32_t id) {
        if (is_constant(id, false)) return constant(true);
        if (is_constant(id, true)) return constant(false);
        if (result_.nodes[id].op == WordcodeOp::Not) return result_.nodes[id].a;
        const auto result = intern({WordcodeOp::Not, id});
        if (atoms_[id].known) {
            atoms_[result] = atoms_[id];
            atoms_[result].inverted = !atoms_[result].inverted;
        }
        return result;
    }

    std::uint32_t greater(std::uint16_t load, std::int32_t threshold) {
        const AtomKey key{load, threshold};
        if (const auto it = comparisons_.find(key); it != comparisons_.end()) return it->second;
        PredicatePlan atom;
        atom.complete = true;
        atom.nodes[0] = {PredicateOp::Greater, 0, 0, 0, threshold};
        atom.node_count = atom.load_count = atom.certificate_count = 1;
        atom.loads[0] = atom.certificates[0] = result_.loads[load];
        atom.root = 0;
        const auto compiled = compile_wordcode(atom, budget_);
        if (!compiled.complete)
            throw Failure{"frontier atomic lowering failed: " + compiled.reason};
        std::vector<std::uint32_t> remap(compiled.nodes.size());
        for (std::size_t i = 0; i < compiled.nodes.size(); ++i) {
            const auto& node = compiled.nodes[i];
            switch (node.op) {
            case WordcodeOp::False: remap[i] = constant(false); break;
            case WordcodeOp::True: remap[i] = constant(true); break;
            case WordcodeOp::Plane:
                if (node.bit >= prefix_bits_)
                    throw Failure{"frontier atom unexpectedly reads an unknown suffix plane"};
                remap[i] = intern({WordcodeOp::Plane, 0, 0,
                    physical_load(compiled.loads[node.load_index].channel), node.bit});
                break;
            case WordcodeOp::And:
            case WordcodeOp::Or: remap[i] = binary(node.op, remap[node.a], remap[node.b]); break;
            case WordcodeOp::Not: remap[i] = negate(remap[node.a]); break;
            }
        }
        const auto result = remap[compiled.root];
        if (!is_constant(result, false) && !is_constant(result, true))
            atoms_[result] = {true, false, load, threshold};
        comparisons_.emplace(key, result);
        return result;
    }

    void finish() {
        // Both outputs are roots of reachability, and both remain live until
        // every node has been evaluated. Keeping only one would be unsound for
        // an arena executor that reuses operation slots.
        std::vector<bool> needed(result_.nodes.size());
        needed[result_.must_root] = needed[result_.may_root] = true;
        for (std::size_t i = result_.nodes.size(); i--;) if (needed[i]) {
            const auto& node = result_.nodes[i];
            if (operation(node.op)) needed[node.a] = true;
            if (node.op == WordcodeOp::And || node.op == WordcodeOp::Or) needed[node.b] = true;
        }
        std::vector<WordcodeNode> nodes;
        std::vector<std::uint32_t> remap(result_.nodes.size());
        for (std::size_t i = 0; i < result_.nodes.size(); ++i) if (needed[i]) {
            auto node = result_.nodes[i];
            if (operation(node.op)) node.a = remap[node.a];
            if (node.op == WordcodeOp::And || node.op == WordcodeOp::Or) node.b = remap[node.b];
            node.slot = no_wordcode_slot;
            remap[i] = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(node);
        }
        result_.must_root = remap[result_.must_root];
        result_.may_root = remap[result_.may_root];
        result_.nodes = std::move(nodes);
        std::vector<std::uint32_t> last(result_.nodes.size()), owners(result_.nodes.size());
        for (std::size_t i = 0; i < result_.nodes.size(); ++i) {
            last[i] = static_cast<std::uint32_t>(i);
            const auto& node = result_.nodes[i];
            if (operation(node.op)) last[node.a] = static_cast<std::uint32_t>(i);
            if (node.op == WordcodeOp::And || node.op == WordcodeOp::Or)
                last[node.b] = static_cast<std::uint32_t>(i);
        }
        last[result_.must_root] = last[result_.may_root] = static_cast<std::uint32_t>(result_.nodes.size());
        for (std::uint32_t i = 0; i < result_.nodes.size(); ++i) {
            auto& node = result_.nodes[i];
            if (node.op == WordcodeOp::Plane) {
                ++result_.plane_count;
                result_.maximum_depth = std::max(result_.maximum_depth, unsigned(node.bit) + 1);
            } else if (operation(node.op)) {
                ++result_.operation_count;
                std::uint32_t slot = 0;
                for (; slot < result_.live_slot_count; ++slot) if (last[owners[slot]] <= i) break;
                if (slot == result_.live_slot_count) ++result_.live_slot_count;
                owners[slot] = i;
                node.slot = slot;
            }
        }
    }
};

bool same_loads(const std::vector<Load>& a, const std::vector<Load>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].channel != b[i].channel || a[i].offset != b[i].offset || a[i].range != b[i].range)
            return false;
    return true;
}

bool same_nodes(const std::vector<WordcodeNode>& a, const std::vector<WordcodeNode>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.op != y.op || x.a != y.a || x.b != y.b || x.load_index != y.load_index ||
            x.bit != y.bit || x.slot != y.slot) return false;
    }
    return true;
}

bool same_stage(const FrontierStage& a, const FrontierStage& b) noexcept {
    return a.must_root == b.must_root && a.may_root == b.may_root &&
        a.plane_count == b.plane_count && a.operation_count == b.operation_count &&
        a.live_slot_count == b.live_slot_count && a.maximum_depth == b.maximum_depth &&
        same_loads(a.loads, b.loads) && same_nodes(a.nodes, b.nodes);
}

bool same_exact(const WordcodePlan& a, const WordcodePlan& b) noexcept {
    return a.complete == b.complete && a.root == b.root &&
        a.plane_count == b.plane_count && a.operation_count == b.operation_count &&
        a.live_slot_count == b.live_slot_count && a.maximum_depth == b.maximum_depth &&
        a.source_predicate_nodes == b.source_predicate_nodes && a.source_comparisons == b.source_comparisons &&
        same_loads(a.loads, b.loads) && same_nodes(a.nodes, b.nodes);
}

} // namespace

FrontierPlan compile_frontier(const PredicatePlan& source, unsigned prefix_bits,
                             std::uint32_t node_budget) {
    if (prefix_bits > 32) throw std::invalid_argument("frontier prefix_bits must be in 0..32");
    if (!node_budget || node_budget > max_wordcode_nodes)
        throw std::invalid_argument("frontier node budget must be in 1..16384");
    try {
        auto exact = compile_wordcode(source, node_budget);
        if (!exact.complete) throw Failure{"frontier exact lowering failed: " + exact.reason};
        auto prefix = StageCompiler(source, exact, prefix_bits, node_budget).compile();
        FrontierPlan result;
        result.complete = true;
        result.prefix_bits = prefix_bits;
        result.prefix = std::move(prefix);
        result.exact = std::move(exact);
        result.source_ = source;
        result.node_budget_ = node_budget;
        return result;
    } catch (const Failure& failure) {
        FrontierPlan result;
        result.prefix_bits = prefix_bits;
        result.reason = failure.reason;
        return result;
    }
}

void validate_frontier(const FrontierPlan& plan) {
    if (!plan.complete || !plan.exact.complete || plan.prefix_bits > 32 ||
        !plan.node_budget_ || plan.node_budget_ > max_wordcode_nodes ||
        plan.prefix.nodes.empty() || plan.prefix.nodes.size() > plan.node_budget_ ||
        plan.exact.nodes.empty() || plan.exact.nodes.size() > plan.node_budget_ ||
        plan.prefix.loads.size() > max_program_nodes || plan.exact.loads.size() > max_program_nodes)
        throw std::invalid_argument("frontier plan is incomplete, unconstructed, or exceeds its retained budget");
    const auto canonical = compile_frontier(plan.source_, plan.prefix_bits, plan.node_budget_);
    if (!canonical.complete || !same_stage(plan.prefix, canonical.prefix) ||
        !same_exact(plan.exact, canonical.exact))
        throw std::invalid_argument("frontier plan differs from its canonical source witness");
}

std::string FrontierPlan::describe() const {
    if (!complete) return "frontier fallback: " + reason;
    std::ostringstream text;
    text << "certified Boolean prefix bounds; prefix_bits=" << prefix_bits
         << "; prefix_nodes=" << prefix.nodes.size() << "; prefix_planes=" << prefix.plane_count
         << "; prefix_operations=" << prefix.operation_count << "; prefix_slots=" << prefix.live_slot_count
         << "; exact_nodes=" << exact.nodes.size() << "; exact_planes=" << exact.plane_count;
    return text.str();
}

} // namespace suffice
