#include "suffice/wordcode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define SUFFICE_WORDCODE_X86 1
#if defined(__GNUC__) || defined(__clang__)
#define SUFFICE_WORDCODE_AVX2 __attribute__((target("avx2")))
#else
#define SUFFICE_WORDCODE_AVX2
#endif
#else
#define SUFFICE_WORDCODE_X86 0
#endif

namespace suffice {
namespace {

struct Failure { std::string reason; };

struct Key {
    WordcodeOp op{};
    std::uint32_t a{}, b{};
    std::uint16_t load{};
    std::uint8_t bit{};
    friend bool operator==(const Key&, const Key&) = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        // Equality is still checked by unordered_map; this is not a proof hash.
        auto value = std::uint64_t(k.a) * 0x9e3779b185ebca87ULL;
        value ^= std::uint64_t(k.b) * 0xc2b2ae3d27d4eb4fULL;
        value ^= std::uint64_t(k.load) * 0x165667b19e3779f9ULL;
        value ^= (std::uint64_t(k.bit) << 8) | static_cast<unsigned>(k.op);
        value ^= value >> 33;
        return static_cast<std::size_t>(value);
    }
};

void validate_predicate(const PredicatePlan& source) {
    if (!source.complete || !source.node_count ||
        source.node_count > max_predicate_nodes || source.load_count > max_program_nodes ||
        source.certificate_count > max_program_nodes ||
        source.root >= source.node_count)
        throw Failure{"wordcode needs a complete valid predicate plan"};
    for (std::size_t i = 0; i < source.certificate_count; ++i) {
        const auto& load = source.certificates[i];
        if (load.offset < -max_input_offset || load.offset > max_input_offset ||
            load.range.lo > load.range.hi ||
            load.range.lo < std::numeric_limits<std::int32_t>::min() ||
            load.range.hi > std::numeric_limits<std::int32_t>::max())
            throw Failure{"wordcode source range certificate is invalid"};
    }
    for (std::size_t i = 0; i < source.load_count; ++i) {
        const auto& load = source.loads[i];
        if (load.offset || load.range.lo > load.range.hi ||
            load.range.lo < std::numeric_limits<std::int32_t>::min() ||
            load.range.hi > std::numeric_limits<std::int32_t>::max())
            throw Failure{"wordcode input contract is invalid"};
    }
    for (std::size_t i = 0; i < source.node_count; ++i) {
        const auto& n = source.nodes[i];
        switch (n.op) {
        case PredicateOp::False: case PredicateOp::True: break;
        case PredicateOp::Greater:
            if (n.load_index >= source.load_count)
                throw Failure{"wordcode predicate load is invalid"};
            break;
        case PredicateOp::And: case PredicateOp::Or:
            if (n.a >= i || n.b >= i)
                throw Failure{"wordcode predicate is not topological"};
            break;
        case PredicateOp::Not:
            if (n.a >= i) throw Failure{"wordcode predicate is not topological"};
            break;
        default: throw Failure{"wordcode predicate opcode is invalid"};
        }
    }
}

bool operation(WordcodeOp op) noexcept {
    return op == WordcodeOp::And || op == WordcodeOp::Or || op == WordcodeOp::Not;
}

// This deterministic layout permits a destination to overwrite an operand at
// that operand's last use. Each loop reads both source words before its store.
std::vector<std::uint32_t> layout(const std::vector<WordcodeNode>& nodes,
                                 std::uint32_t root, std::uint32_t& count) {
    std::vector<std::uint32_t> last(nodes.size()), owners(nodes.size()), slots(nodes.size(), no_wordcode_slot);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        last[i] = static_cast<std::uint32_t>(i);
        const auto& n = nodes[i];
        if (operation(n.op)) last[n.a] = static_cast<std::uint32_t>(i);
        if (n.op == WordcodeOp::And || n.op == WordcodeOp::Or)
            last[n.b] = static_cast<std::uint32_t>(i);
    }
    last[root] = static_cast<std::uint32_t>(nodes.size());
    count = 0;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        if (!operation(nodes[i].op)) continue;
        std::uint32_t slot = 0;
        for (; slot < count; ++slot) if (last[owners[slot]] <= i) break;
        if (slot == count) ++count;
        owners[slot] = i;
        slots[i] = slot;
    }
    return slots;
}

class Compiler {
public:
    Compiler(const PredicatePlan& source, std::uint32_t budget)
        : source_(source), budget_(budget) {}

    WordcodePlan compile() {
        validate_predicate(source_);
        result_.source_predicate_nodes = source_.node_count;
        // Several contracts for the same physical channel must all hold. Their
        // intersection remains a valid certificate and gives one plane identity.
        for (std::size_t i = 0; i < source_.certificate_count; ++i)
            (void)merge_load(source_.certificates[i]);
        for (std::uint16_t i = 0; i < source_.load_count; ++i) {
            load_map_[i] = merge_load(source_.loads[i]);
        }
        std::array<std::uint32_t, max_predicate_nodes> mapped{};
        for (std::size_t i = 0; i < source_.node_count; ++i) {
            const auto& n = source_.nodes[i];
            switch (n.op) {
            case PredicateOp::False: mapped[i] = constant(false); break;
            case PredicateOp::True: mapped[i] = constant(true); break;
            case PredicateOp::Greater:
                ++result_.source_comparisons;
                mapped[i] = greater(load_map_[n.load_index], n.threshold); break;
            case PredicateOp::And:
                mapped[i] = binary(WordcodeOp::And, mapped[n.a], mapped[n.b]); break;
            case PredicateOp::Or:
                mapped[i] = binary(WordcodeOp::Or, mapped[n.a], mapped[n.b]); break;
            case PredicateOp::Not: mapped[i] = negate(mapped[n.a]); break;
            }
        }
        result_.root = mapped[source_.root];
        prune();
        const auto slots = layout(result_.nodes, result_.root, result_.live_slot_count);
        for (std::size_t i = 0; i < result_.nodes.size(); ++i) {
            auto& n = result_.nodes[i];
            n.slot = slots[i];
            if (n.op == WordcodeOp::Plane) {
                ++result_.plane_count;
                result_.maximum_depth = std::max(result_.maximum_depth, unsigned(n.bit) + 1);
            } else if (operation(n.op)) ++result_.operation_count;
        }
        result_.complete = true;
        return std::move(result_);
    }

private:
    struct Atom {
        bool known{}, inverted{};
        std::uint16_t load{};
        std::int32_t threshold{};
    };
    const PredicatePlan& source_;
    std::uint32_t budget_;
    WordcodePlan result_;
    std::array<std::uint16_t, max_program_nodes> load_map_{};
    std::unordered_map<Key, std::uint32_t, KeyHash> interned_;
    std::vector<Atom> atoms_;

    std::uint16_t merge_load(const Load& input) {
        auto found = std::find_if(result_.loads.begin(), result_.loads.end(),
            [&](const Load& load) { return load.channel == input.channel; });
        if (found == result_.loads.end()) {
            if (result_.loads.size() >= max_program_nodes)
                throw Failure{"wordcode unique channel certificate budget exceeded"};
            const auto index = static_cast<std::uint16_t>(result_.loads.size());
            // A certificate constrains every value of its physical channel;
            // its original halo offset does not change that range property.
            result_.loads.push_back({input.channel, 0, input.range});
            return index;
        }
        found->range.lo = std::max(found->range.lo, input.range.lo);
        found->range.hi = std::min(found->range.hi, input.range.hi);
        if (found->range.lo > found->range.hi)
            throw Failure{"wordcode channel contracts have empty intersection"};
        return static_cast<std::uint16_t>(found - result_.loads.begin());
    }

    std::uint32_t intern(Key key) {
        if (const auto it = interned_.find(key); it != interned_.end()) return it->second;
        if (result_.nodes.size() >= budget_)
            throw Failure{"wordcode node budget exhausted; no partial circuit is executable"};
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
        const auto& n = result_.nodes[id];
        return n.op == op && (n.a == child || n.b == child);
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
                // A greater threshold is the stronger > condition, but the
                // weaker <= condition. Keep the appropriate existing circuit.
                const bool want_larger = is_and != left.inverted;
                return (left.threshold >= right.threshold) == want_larger ? a : b;
            }
            const auto greater = left.inverted ? right.threshold : left.threshold;
            const auto upper = left.inverted ? left.threshold : right.threshold;
            if (is_and && greater >= upper) return constant(false);
            if (!is_and && greater <= upper) return constant(true);
        }
        const auto other = is_and ? WordcodeOp::Or : WordcodeOp::And;
        if (contains(a, other, b)) return b;
        if (contains(b, other, a)) return a;
        if (a > b) std::swap(a, b);
        return intern({op, a, b});
    }
    std::uint32_t negate(std::uint32_t a) {
        if (is_constant(a, false)) return constant(true);
        if (is_constant(a, true)) return constant(false);
        if (result_.nodes[a].op == WordcodeOp::Not) return result_.nodes[a].a;
        const auto id = intern({WordcodeOp::Not, a});
        if (atoms_[a].known) {
            atoms_[id] = atoms_[a];
            atoms_[id].inverted = !atoms_[id].inverted;
        }
        return id;
    }
    std::uint32_t plane(std::uint16_t load, unsigned numeric_bit) {
        const auto& range = result_.loads[load].range;
        const auto lower = std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(range.lo)) ^ 0x80000000U;
        const auto upper = std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(range.hi)) ^ 0x80000000U;
        // A bit is constant on an integer interval exactly when no boundary of
        // a 2^bit-sized run is crossed. Equal endpoint bits alone are not enough.
        if ((lower >> numeric_bit) == (upper >> numeric_bit))
            return constant(((lower >> numeric_bit) & 1U) != 0);
        return intern({WordcodeOp::Plane, 0, 0, load,
                       static_cast<std::uint8_t>(31U - numeric_bit)});
    }
    std::uint32_t greater(std::uint16_t load, std::int32_t threshold) {
        const auto range = result_.loads[load].range;
        if (range.lo > threshold) return constant(true);
        if (range.hi <= threshold) return constant(false);
        const auto biased = std::bit_cast<std::uint32_t>(threshold) ^ 0x80000000U;
        // Start at the first zero of k: every lower one would compute x_i & 0.
        const auto trailing_ones = static_cast<unsigned>(std::countr_one(biased));
        if (trailing_ones == 32) return constant(false);
        auto result = plane(load, trailing_ones);
        for (unsigned bit = trailing_ones + 1; bit < 32; ++bit)
            result = binary((biased & (std::uint32_t{1} << bit)) ? WordcodeOp::And : WordcodeOp::Or,
                            plane(load, bit), result);
        if (!is_constant(result, false) && !is_constant(result, true))
            atoms_[result] = {true, false, load, threshold};
        return result;
    }
    void prune() {
        std::vector<bool> needed(result_.nodes.size());
        needed[result_.root] = true;
        for (std::size_t i = result_.nodes.size(); i--;) if (needed[i]) {
            const auto& n = result_.nodes[i];
            if (operation(n.op)) needed[n.a] = true;
            if (n.op == WordcodeOp::And || n.op == WordcodeOp::Or) needed[n.b] = true;
        }
        std::vector<WordcodeNode> nodes;
        std::vector<std::uint32_t> remap(result_.nodes.size());
        for (std::size_t i = 0; i < result_.nodes.size(); ++i) if (needed[i]) {
            auto n = result_.nodes[i];
            if (operation(n.op)) n.a = remap[n.a];
            if (n.op == WordcodeOp::And || n.op == WordcodeOp::Or) n.b = remap[n.b];
            remap[i] = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(n);
        }
        result_.root = remap[result_.root];
        result_.nodes = std::move(nodes);
        // Retain all source load certificates. A deleted plane may have become
        // constant only under its range assumption; dropping that assumption
        // would let a low-level caller execute the folded circuit on invalid data.
    }
};

void validate_wordcode(const WordcodePlan& plan) {
    if (!plan.complete || plan.nodes.empty() || plan.nodes.size() > max_wordcode_nodes ||
        plan.root >= plan.nodes.size() || plan.loads.size() > max_program_nodes)
        throw std::invalid_argument("wordcode plan is incomplete or invalid");
    unsigned depth = 0;
    std::uint32_t planes = 0, operations = 0;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& n = plan.nodes[i];
        switch (n.op) {
        case WordcodeOp::False: case WordcodeOp::True: break;
        case WordcodeOp::Plane:
            if (n.load_index >= plan.loads.size() || n.bit >= 32)
                throw std::invalid_argument("wordcode plane is invalid");
            ++planes; depth = std::max(depth, unsigned(n.bit) + 1); break;
        case WordcodeOp::And: case WordcodeOp::Or:
            if (n.a >= i || n.b >= i)
                throw std::invalid_argument("wordcode graph is not topological");
            ++operations; break;
        case WordcodeOp::Not:
            if (n.a >= i) throw std::invalid_argument("wordcode graph is not topological");
            ++operations; break;
        default: throw std::invalid_argument("wordcode opcode is invalid");
        }
    }
    std::uint32_t slots{};
    const auto expected = layout(plan.nodes, plan.root, slots);
    if (slots != plan.live_slot_count || planes != plan.plane_count ||
        operations != plan.operation_count || depth != plan.maximum_depth)
        throw std::invalid_argument("wordcode plan statistics or layout are invalid");
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (expected[i] != plan.nodes[i].slot)
            throw std::invalid_argument("wordcode live-slot assignment is invalid");
}

std::uint64_t load_full(const std::uint8_t* bytes) noexcept {
    std::uint64_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
std::uint64_t low_mask(unsigned bits) noexcept {
    return bits == 64 ? UINT64_MAX : ((std::uint64_t{1} << bits) - 1);
}

#if SUFFICE_WORDCODE_X86
SUFFICE_WORDCODE_AVX2 std::size_t operator_avx2(
    WordcodeOp op, const std::uint8_t* a, const std::uint8_t* b,
    std::uint64_t* destination, std::size_t words) noexcept {
    std::size_t i = 0;
    if (op == WordcodeOp::And) {
        for (; i + 4 <= words; i += 4) {
            const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), _mm256_and_si256(left, right));
        }
    } else if (op == WordcodeOp::Or) {
        for (; i + 4 <= words; i += 4) {
            const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), _mm256_or_si256(left, right));
        }
    } else {
        const auto ones = _mm256_set1_epi64x(-1);
        for (; i + 4 <= words; i += 4) {
            const auto value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i * 8));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), _mm256_xor_si256(value, ones));
        }
    }
    return i;
}
#endif

} // namespace

WordcodePlan compile_wordcode(const PredicatePlan& source, std::uint32_t budget) {
    if (!budget || budget > max_wordcode_nodes)
        throw std::invalid_argument("wordcode node budget must be in 1..16384");
    try { return Compiler(source, budget).compile(); }
    catch (const Failure& failure) {
        WordcodePlan result;
        result.reason = failure.reason;
        return result;
    }
}

std::string WordcodePlan::describe() const {
    if (!complete) return "wordcode fallback: " + reason;
    std::ostringstream text;
    text << "constant-threshold Boolean circuit; nodes=" << nodes.size()
         << "; planes=" << plane_count << "; operations=" << operation_count
         << "; slots=" << live_slot_count << "; depth=" << maximum_depth
         << "; source_comparisons=" << source_comparisons;
    return text.str();
}

WordcodeExecutor::WordcodeExecutor(const PackedDataset& data, const WordcodePlan& plan,
                                 std::size_t tile_words, bool allow_simd)
    : data_(data), plan_(plan), tile_words_(tile_words) {
    validate_wordcode(plan);
    if (!tile_words || tile_words > 4096)
        throw std::invalid_argument("wordcode tile_words must be in 1..4096");
#if SUFFICE_WORDCODE_X86
    use_simd_ = allow_simd && std::endian::native == std::endian::little && avx2_available();
#else
    (void)allow_simd;
#endif
    for (const auto& load : plan.loads) {
        if (load.offset || load.range.lo > load.range.hi ||
            load.range.lo < std::numeric_limits<std::int32_t>::min() ||
            load.range.hi > std::numeric_limits<std::int32_t>::max() ||
            load.channel >= data.info().channels)
            throw std::invalid_argument("wordcode input contract is invalid");
        const auto range = data.info().ranges[load.channel];
        if (load.range.lo > range.min || load.range.hi < range.max)
            throw std::invalid_argument("wordcode range does not cover packed dataset");
    }
    arena_.resize(std::size_t(plan.live_slot_count) * tile_words);
    views_.resize(plan.nodes.size());
    planes_.resize(plan.nodes.size());
}

std::uint64_t WordcodeExecutor::required_scratch_bytes(const WordcodePlan& plan,
                                                     std::size_t tile_words) noexcept {
    if (!plan.complete || tile_words > 4096 || plan.nodes.size() > max_wordcode_nodes ||
        plan.live_slot_count > max_wordcode_nodes) return 0;
    return sizeof(WordcodeExecutor) + std::uint64_t(plan.live_slot_count) * tile_words * 8 +
           std::uint64_t(plan.nodes.size()) * (sizeof(View) + sizeof(std::span<const std::uint8_t>));
}
std::uint64_t WordcodeExecutor::scratch_bytes() const noexcept {
    return sizeof(*this) + arena_.capacity() * sizeof(std::uint64_t) +
           views_.capacity() * sizeof(View) + planes_.capacity() * sizeof(std::span<const std::uint8_t>);
}
std::string WordcodeExecutor::description() const {
    return "encoded eager operator batches; tile_words=" + std::to_string(tile_words_) +
           (use_simd_ ? "; AVX2 with scalar tails; " : "; scalar; ") + plan_.describe();
}

std::uint64_t WordcodeExecutor::word(const View& view, std::size_t index) noexcept {
    if (view.native_words) return view.native_words[index];
    if (!view.bytes) return view.constant;
    const auto offset = index * 8;
    if (offset >= view.size) return 0;
    const auto size = std::min<std::size_t>(8, view.size - offset);
    std::uint64_t value = 0;
    if constexpr (std::endian::native == std::endian::little)
        std::memcpy(&value, view.bytes + offset, size);
    else
        for (std::size_t b = 0; b < size; ++b)
            value |= std::uint64_t(view.bytes[offset + b]) << (8 * b);
    if (view.shift) {
        value >>= view.shift;
        if (offset + 8 < view.size)
            value |= std::uint64_t(view.bytes[offset + 8]) << (64 - view.shift);
    }
    return value;
}

ExecutionStats WordcodeExecutor::run(std::span<std::uint64_t> output) {
    return run_range(0, data_.info().rows, output);
}

ExecutionStats WordcodeExecutor::run_range(std::uint64_t first, std::uint64_t count,
                                         std::span<std::uint64_t> output) {
    const auto rows = data_.info().rows;
    if (first > rows || count > rows - first)
        throw std::out_of_range("wordcode row range is outside dataset");
    if (output.size() != mask_words(count))
        throw std::invalid_argument("wordcode output must exactly match range mask size");
    std::fill(output.begin(), output.end(), std::uint64_t{0});
    ExecutionStats stats;
    stats.rows = count;
    stats.output_bytes = output.size_bytes();
    stats.peak_scratch_bytes = scratch_bytes();
    stats.kernel = "encoded-wordcode-scalar";
    if (!count) return stats;
    const auto root_op = plan_.nodes[plan_.root].op;
    if (root_op == WordcodeOp::True || root_op == WordcodeOp::False) {
        stats.kernel = "encoded-wordcode-constant";
        if (root_op == WordcodeOp::True) {
            std::fill(output.begin(), output.end(), UINT64_MAX);
            if (count % 64) output.back() = low_mask(static_cast<unsigned>(count % 64));
            stats.accepted = count;
        }
        stats.resolved_by_bits[0] = count;
        stats.bulk_resolutions = 1;
        return stats;
    }
    const auto block_rows = data_.info().block_rows;
    std::uint64_t produced = 0;
    while (produced < count) {
        const auto global = first + produced;
        const auto block = global / block_rows;
        const auto local_first = static_cast<std::size_t>(global % block_rows);
        const auto segment = static_cast<std::size_t>(std::min<std::uint64_t>(
            count - produced, data_.block_length(block) - local_first));
        const auto byte_first = local_first / 8;
        const auto byte_count = (local_first % 8 + segment + 7) / 8;
        ++stats.region_loads;
        for (std::size_t i = 0; i < plan_.nodes.size(); ++i) {
            const auto& n = plan_.nodes[i];
            if (n.op != WordcodeOp::Plane) continue;
            planes_[i] = data_.plane_view(block, plan_.loads[n.load_index].channel, n.bit, 1);
            ++stats.loaded_planes;
            stats.requested_payload_bytes += byte_count;
            const auto alignment = reinterpret_cast<std::uintptr_t>(planes_[i].data() + byte_first) % 64;
            stats.estimated_cacheline_bytes += ((alignment + byte_count + 63) / 64) * 64;
        }
        std::size_t consumed = 0;
        while (consumed < segment) {
            const auto take = std::min(segment - consumed, tile_words_ * 64);
            const auto local = local_first + consumed;
            const auto words = (take + 63) / 64;
            const auto full_words = take / 64;
            for (std::size_t i = 0; i < plan_.nodes.size(); ++i) {
                const auto& n = plan_.nodes[i];
                auto& view = views_[i];
                if (n.op == WordcodeOp::False || n.op == WordcodeOp::True) {
                    view = {};
                    view.constant = n.op == WordcodeOp::True ? UINT64_MAX : 0;
                    continue;
                }
                if (n.op == WordcodeOp::Plane) {
                    view = {};
                    view.bytes = planes_[i].data() + local / 8;
                    view.shift = static_cast<unsigned>(local % 8);
                    view.size = (view.shift + take + 7) / 8;
                    continue;
                }
                const auto a = views_[n.a];
                const auto b = n.op == WordcodeOp::Not ? View{} : views_[n.b];
                auto* destination = arena_.data() + std::size_t(n.slot) * tile_words_;
                std::size_t processed = 0;
                if constexpr (std::endian::native == std::endian::little) {
                    const auto* ap = a.native_words ? reinterpret_cast<const std::uint8_t*>(a.native_words) : a.bytes;
                    const auto* bp = b.native_words ? reinterpret_cast<const std::uint8_t*>(b.native_words) : b.bytes;
                    if (ap && !a.shift && (n.op == WordcodeOp::Not || (bp && !b.shift))) {
#if SUFFICE_WORDCODE_X86
                        if (use_simd_ && full_words >= 4) {
                            processed = operator_avx2(n.op, ap, bp, destination, full_words);
                            stats.kernel = "encoded-wordcode-avx2";
                        }
#endif
                        if (n.op == WordcodeOp::And)
                            for (; processed < full_words; ++processed)
                                destination[processed] = load_full(ap + 8 * processed) & load_full(bp + 8 * processed);
                        else if (n.op == WordcodeOp::Or)
                            for (; processed < full_words; ++processed)
                                destination[processed] = load_full(ap + 8 * processed) | load_full(bp + 8 * processed);
                        else
                            for (; processed < full_words; ++processed)
                                destination[processed] = ~load_full(ap + 8 * processed);
                    }
                }
                if (n.op == WordcodeOp::And)
                    for (; processed < words; ++processed)
                        destination[processed] = word(a, processed) & word(b, processed);
                else if (n.op == WordcodeOp::Or)
                    for (; processed < words; ++processed)
                        destination[processed] = word(a, processed) | word(b, processed);
                else
                    for (; processed < words; ++processed)
                        destination[processed] = ~word(a, processed);
                view = {};
                view.native_words = destination;
            }
            const auto root = views_[plan_.root];
            const auto output_first = produced + consumed;
            const auto shift = static_cast<unsigned>(output_first % 64);
            auto output_index = static_cast<std::size_t>(output_first / 64);
            for (std::size_t i = 0; i < words; ++i, ++output_index) {
                const auto valid = static_cast<unsigned>(std::min<std::size_t>(64, take - i * 64));
                const auto value = word(root, i) & low_mask(valid);
                output[output_index] |= value << shift;
                if (shift && valid > 64 - shift)
                    output[output_index + 1] |= value >> (64 - shift);
                stats.accepted += std::popcount(value);
            }
            ++stats.bulk_resolutions;
            consumed += take;
        }
        produced += segment;
    }
    stats.resolved_by_bits[plan_.maximum_depth] = count;
    stats.refined_rows = plan_.maximum_depth > 4 ? count : 0;
    return stats;
}

} // namespace suffice
