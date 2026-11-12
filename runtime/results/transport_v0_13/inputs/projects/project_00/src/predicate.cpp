#include "suffice/predicate.hpp"
#include "suffice/row_range.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SUFFICE_PREDICATE_X86 1
#include <immintrin.h>
#else
#define SUFFICE_PREDICATE_X86 0
#endif
#if SUFFICE_PREDICATE_X86 && (defined(__GNUC__) || defined(__clang__))
#define SUFFICE_PREDICATE_AVX2 __attribute__((target("avx2")))
#else
#define SUFFICE_PREDICATE_AVX2
#endif

namespace suffice {
namespace {
constexpr i64 lowest = std::numeric_limits<i64>::min();
constexpr i64 highest = std::numeric_limits<i64>::max();
struct LoweringFailure { const char* reason; };

i64 subtract(i64 a, i64 b) {
    if ((b > 0 && a < lowest + b) || (b < 0 && a > highest + b))
        throw LoweringFailure{"inverse threshold exceeded int64"};
    return a - b;
}
i64 add(i64 a, i64 b) {
    if ((b > 0 && a > highest - b) || (b < 0 && a < lowest - b))
        throw LoweringFailure{"inverse threshold exceeded int64"};
    return a + b;
}
i64 floor_div(i64 a, i64 b) {
    if (!b || (a == lowest && b == -1))
        throw LoweringFailure{"inverse division exceeded int64"};
    const i64 q = a / b, r = a % b;
    return q - (r != 0 && ((r < 0) != (b < 0)) ? 1 : 0);
}
i64 ceil_div(i64 a, i64 b) {
    if (!b || (a == lowest && b == -1))
        throw LoweringFailure{"inverse division exceeded int64"};
    const i64 q = a / b, r = a % b;
    return q + (r != 0 && ((r < 0) == (b < 0)) ? 1 : 0);
}
i64 integer_sqrt(i64 value) noexcept {
    // Called only for nonnegative thresholds. Division avoids square overflow.
    std::uint64_t lo = 0, hi = 3037000499ULL;
    const auto n = static_cast<std::uint64_t>(value);
    while (lo < hi) {
        const auto middle = lo + (hi - lo + 1) / 2;
        if (middle <= n / middle) lo = middle;
        else hi = middle - 1;
    }
    return static_cast<i64>(lo);
}
std::uint64_t low_mask(unsigned bits) noexcept {
    return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}
void set_bits(std::span<std::uint64_t> out, std::uint64_t first,
              std::uint64_t bits, unsigned count) noexcept {
    bits &= low_mask(count);
    const auto word = static_cast<std::size_t>(first / 64);
    const auto shift = static_cast<unsigned>(first % 64);
    out[word] |= bits << shift;
    if (shift && count > 64 - shift) out[word + 1] |= bits >> (64 - shift);
}
std::int32_t raw_value(const std::uint8_t* bytes, std::uint64_t index) noexcept {
    const auto* p = bytes + index * 4;
    const auto word = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                      (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    return std::bit_cast<std::int32_t>(word);
}

class Lowerer {
public:
    Lowerer(const CompiledProgram& source, std::uint32_t budget)
        : source_(source), budget_(budget) { memo_.reserve(max_predicate_lowering_steps); }
    PredicatePlan compile() {
        if (!source_.node_count()) throw LoweringFailure{"source program is not compiled"};
        if (!budget_ || budget_ > max_predicate_nodes)
            throw LoweringFailure{"Boolean node budget must be in 1..256"};
        for (const auto& load : source_.loads()) if (load.offset)
            throw LoweringFailure{"predicate lowering requires unshifted inputs"};
        result_.certificate_count = static_cast<std::uint16_t>(source_.loads().size());
        std::copy(source_.loads().begin(), source_.loads().end(), result_.certificates.begin());
        result_.root = greater(source_.output_node(), source_.threshold());
        prune();
        result_.complete = true;
        result_.reason = "complete";
        return std::move(result_);
    }
    std::uint32_t steps() const noexcept { return result_.lowering_steps; }
private:
    struct Memo { NodeId source; i64 threshold; PredicateNodeId result; };
    const CompiledProgram& source_;
    std::uint32_t budget_;
    PredicatePlan result_;
    std::vector<Memo> memo_;

    void prune() {
        std::array<bool,max_predicate_nodes> needed{};
        needed[result_.root]=true;
        for(std::size_t i=result_.node_count;i--;)if(needed[i]) {
            const auto& n=result_.nodes[i];
            if(n.op==PredicateOp::And||n.op==PredicateOp::Or)needed[n.a]=needed[n.b]=true;
            else if(n.op==PredicateOp::Not)needed[n.a]=true;
        }
        std::array<PredicateNodeId,max_predicate_nodes> map{};
        std::array<std::uint16_t,max_program_nodes> load_map{};
        load_map.fill(std::numeric_limits<std::uint16_t>::max());
        std::array<Load,max_program_nodes> loads{};
        std::uint16_t nodes=0,load_count=0;
        for(std::uint16_t i=0;i<result_.node_count;++i)if(needed[i]) {
            auto n=result_.nodes[i];
            if(n.op==PredicateOp::And||n.op==PredicateOp::Or) {n.a=map[n.a];n.b=map[n.b];}
            else if(n.op==PredicateOp::Not)n.a=map[n.a];
            else if(n.op==PredicateOp::Greater) {
                if(load_map[n.load_index]==std::numeric_limits<std::uint16_t>::max()) {
                    load_map[n.load_index]=load_count;loads[load_count++]=result_.loads[n.load_index];
                }
                n.load_index=load_map[n.load_index];
            }
            map[i]=nodes;result_.nodes[nodes++]=n;
        }
        result_.root=map[result_.root];result_.node_count=nodes;
        result_.load_count=load_count;result_.loads=loads;
        std::fill(result_.nodes.begin()+nodes,result_.nodes.end(),PredicateNode{});
    }

    PredicateNodeId intern(PredicateNode node) {
        for (std::uint16_t i = 0; i < result_.node_count; ++i)
            if (result_.nodes[i] == node) return i;
        if (result_.node_count >= budget_)
            throw LoweringFailure{"Boolean node budget exhausted"};
        const auto id = result_.node_count++;
        result_.nodes[id] = node;
        return id;
    }
    PredicateNodeId constant(bool value) {
        return intern({value ? PredicateOp::True : PredicateOp::False, 0, 0, 0, 0});
    }
    PredicateNodeId negate(PredicateNodeId a) {
        const auto n = result_.nodes[a];
        if (n.op == PredicateOp::False) return constant(true);
        if (n.op == PredicateOp::True) return constant(false);
        if (n.op == PredicateOp::Not) return n.a;
        return intern({PredicateOp::Not, a, 0, 0, 0});
    }
    PredicateNodeId combine(PredicateOp op, PredicateNodeId a, PredicateNodeId b) {
        if (a == b) return a;
        const auto left = result_.nodes[a].op, right = result_.nodes[b].op;
        if (op == PredicateOp::And) {
            if (left == PredicateOp::False || right == PredicateOp::False) return constant(false);
            if (left == PredicateOp::True) return b;
            if (right == PredicateOp::True) return a;
        } else {
            if (left == PredicateOp::True || right == PredicateOp::True) return constant(true);
            if (left == PredicateOp::False) return b;
            if (right == PredicateOp::False) return a;
        }
        if ((left == PredicateOp::Not && result_.nodes[a].a == b) ||
            (right == PredicateOp::Not && result_.nodes[b].a == a))
            return constant(op == PredicateOp::Or);
        if (a > b) std::swap(a, b);
        return intern({op, a, b, 0, 0});
    }
    std::uint16_t load_index(std::uint16_t source_index) {
        const auto& load = source_.loads()[source_index];
        for (std::uint16_t i = 0; i < result_.load_count; ++i)
            if (result_.loads[i].channel == load.channel && result_.loads[i].range == load.range)
                return i;
        if (result_.load_count == max_program_nodes)
            throw LoweringFailure{"predicate load budget exhausted"};
        const auto id = result_.load_count++;
        result_.loads[id] = load;
        return id;
    }
    PredicateNodeId absolute_greater(NodeId a, i64 threshold) {
        if (threshold < 0) return constant(true);
        const auto positive = greater(a, threshold);
        const auto nonnegative_limit = negate(greater(a, subtract(-threshold, 1)));
        return combine(PredicateOp::Or, positive, nonnegative_limit);
    }
    PredicateNodeId greater(NodeId source_id, i64 threshold) {
        if (++result_.lowering_steps > max_predicate_lowering_steps)
            throw LoweringFailure{"inverse-threshold demand budget exhausted"};
        const auto& n = source_.nodes()[source_id];
        if (n.range.lo > threshold) return constant(true);
        if (n.range.hi <= threshold) return constant(false);
        for (const auto& m : memo_) if (m.source == source_id && m.threshold == threshold) return m.result;
        PredicateNodeId result{};
        switch (n.op) {
        case Op::Load:
            if (threshold < std::numeric_limits<std::int32_t>::min() ||
                threshold > std::numeric_limits<std::int32_t>::max())
                throw LoweringFailure{"unresolved threshold outside int32 load domain"};
            result = intern({PredicateOp::Greater, 0, 0, load_index(n.load_index), static_cast<std::int32_t>(threshold)});
            break;
        case Op::Constant: result = constant(n.immediate > threshold); break;
        case Op::Min: {
            const auto a = greater(n.a, threshold), b = greater(n.b, threshold);
            result = combine(PredicateOp::And, a, b); break;
        }
        case Op::Max: {
            const auto a = greater(n.a, threshold), b = greater(n.b, threshold);
            result = combine(PredicateOp::Or, a, b); break;
        }
        case Op::Abs: result = absolute_greater(n.a, threshold); break;
        case Op::Square: result = absolute_greater(n.a, integer_sqrt(threshold)); break;
        case Op::MulConst:
            if (n.immediate > 0) result = greater(n.a, floor_div(threshold, n.immediate));
            else if (n.immediate < 0)
                result = negate(greater(n.a, subtract(ceil_div(threshold, n.immediate), 1)));
            else result = constant(0 > threshold);
            break;
        case Op::Add: {
            const auto a = source_.nodes()[n.a].range, b = source_.nodes()[n.b].range;
            if (b.lo == b.hi) result = greater(n.a, subtract(threshold, b.lo));
            else if (a.lo == a.hi) result = greater(n.b, subtract(threshold, a.lo));
            else throw LoweringFailure{"addition of two varying values has no Boolean lowering"};
            break;
        }
        case Op::Sub: {
            const auto a = source_.nodes()[n.a].range, b = source_.nodes()[n.b].range;
            if (b.lo == b.hi) result = greater(n.a, add(threshold, b.lo));
            else if (a.lo == a.hi) result = negate(greater(n.b, subtract(subtract(a.lo, threshold), 1)));
            else throw LoweringFailure{"subtraction of two varying values has no Boolean lowering"};
            break;
        }
        }
        memo_.push_back({source_id, threshold, result});
        return result;
    }
};

void validate_plan(const PredicatePlan& plan) {
    if (!plan.complete || !plan.node_count || plan.node_count > max_predicate_nodes ||
        plan.load_count > max_program_nodes || plan.certificate_count > max_program_nodes ||
        plan.root >= plan.node_count)
        throw std::invalid_argument("predicate executor requires a complete bounded plan");
    for (std::size_t i = 0; i < plan.node_count; ++i) {
        const auto& n = plan.nodes[i];
        switch (n.op) {
        case PredicateOp::False: case PredicateOp::True: break;
        case PredicateOp::Greater:
            if (n.load_index >= plan.load_count) throw std::invalid_argument("predicate leaf load is invalid");
            break;
        case PredicateOp::And: case PredicateOp::Or:
            if (n.a >= i || n.b >= i) throw std::invalid_argument("predicate graph is not topological");
            break;
        case PredicateOp::Not:
            if (n.a >= i) throw std::invalid_argument("predicate negation is not topological");
            break;
        default: throw std::invalid_argument("predicate opcode is invalid");
        }
    }
    for (std::size_t i = 0; i < plan.load_count; ++i) {
        const auto& load = plan.loads[i];
        if (load.offset || load.range.lo > load.range.hi ||
            load.range.lo < std::numeric_limits<std::int32_t>::min() ||
            load.range.hi > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("predicate load contract is invalid");
    }
    for (std::size_t i = 0; i < plan.certificate_count; ++i) {
        const auto& load = plan.certificates[i];
        // A certificate constrains its entire channel, independently of the
        // original row offset. Executable leaves must still be unshifted.
        if (load.offset < -max_input_offset || load.offset > max_input_offset ||
            load.range.lo > load.range.hi ||
            load.range.lo < std::numeric_limits<std::int32_t>::min() ||
            load.range.hi > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("predicate range certificate is invalid");
    }
}

constexpr std::size_t sign_tile_words=256;
constexpr auto no_sign_slot=std::numeric_limits<std::uint16_t>::max();
struct SignLayout {
    bool valid{};
    std::uint16_t slot_count{};
    std::array<std::uint16_t,max_predicate_nodes> slots{};
};
SignLayout sign_layout(const PredicatePlan& plan) noexcept {
    SignLayout result;result.slots.fill(no_sign_slot);
    if constexpr(std::endian::native!=std::endian::little)return result;
    if(!plan.complete||!plan.node_count||plan.node_count>max_predicate_nodes||plan.root>=plan.node_count)return result;
    std::array<std::uint16_t,max_predicate_nodes> last_use{},owner{};
    for(std::uint16_t i=0;i<plan.node_count;++i)last_use[i]=i;
    for(std::uint16_t i=0;i<plan.node_count;++i) {
        const auto& n=plan.nodes[i];
        switch(n.op) {
        case PredicateOp::Greater:
            if(n.threshold!=-1||n.load_index>=plan.load_count)return result;
            break;
        case PredicateOp::And:case PredicateOp::Or:
            if(n.a>=i||n.b>=i)return result;
            last_use[n.a]=last_use[n.b]=i;break;
        case PredicateOp::Not:
            if(n.a>=i)return result;
            last_use[n.a]=i;break;
        case PredicateOp::False:case PredicateOp::True:break;
        default:return result;
        }
    }
    last_use[plan.root]=plan.node_count;
    for(std::uint16_t i=0;i<plan.node_count;++i) {
        if(plan.nodes[i].op==PredicateOp::Greater)continue;
        std::uint16_t slot=0;
        // At last use, a source slot can become this operation's destination:
        // each word reads its operands before storing the replacement word.
        for(;slot<result.slot_count;++slot)if(last_use[owner[slot]]<=i)break;
        if(slot==result.slot_count)++result.slot_count;
        owner[slot]=i;result.slots[i]=slot;
    }
    result.valid=true;return result;
}
std::uint64_t full_sign_word(const std::uint8_t* bytes) noexcept {
    std::uint64_t value;std::memcpy(&value,bytes,8);return value;
}
std::uint64_t partial_sign_word(const std::uint8_t* bytes,std::size_t size,std::size_t word) noexcept {
    const auto offset=word*8;const auto count=std::min<std::size_t>(8,size-offset);
    std::uint64_t value=0;std::memcpy(&value,bytes+offset,count);return value;
}

#if SUFFICE_PREDICATE_X86
SUFFICE_PREDICATE_AVX2 std::uint64_t dense_compare_avx2(
    const std::uint8_t* bytes, std::uint64_t first, unsigned count,
    std::uint32_t channels, std::uint32_t channel, std::int32_t threshold,
    std::uint64_t active) noexcept {
    std::uint64_t result = 0;
    const auto t = _mm256_set1_epi32(threshold);
    const auto stride = static_cast<int>(channels);
    const auto indices = _mm256_setr_epi32(0, stride, 2*stride, 3*stride, 4*stride, 5*stride, 6*stride, 7*stride);
    unsigned local = 0;
    for (; local + 8 <= count; local += 8) {
        if (((active >> local) & 255U) == 0) continue;
        const auto* base = reinterpret_cast<const int*>(bytes + ((first+local)*channels+channel)*4);
        const auto value = channels == 1 ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(base))
                                        : _mm256_i32gather_epi32(base, indices, 4);
        const auto bits = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(value, t))));
        result |= std::uint64_t{bits} << local;
    }
    for (; local < count; ++local)
        if ((active & (std::uint64_t{1} << local)) && raw_value(bytes, (first+local)*channels+channel) > threshold)
            result |= std::uint64_t{1} << local;
    return result & active;
}
#endif
} // namespace

PredicatePlan compile_predicate(const CompiledProgram& program, std::uint32_t node_budget) {
    Lowerer lowerer(program, node_budget);
    try { return lowerer.compile(); }
    catch (const LoweringFailure& failure) {
        PredicatePlan result;
        result.reason = failure.reason;
        result.lowering_steps = lowerer.steps();
        return result;
    }
}

std::string PredicatePlan::describe() const {
    if (!complete) return "Boolean predicate fallback: " + reason;
    std::size_t leaves = 0;
    for (std::size_t i = 0; i < node_count; ++i) leaves += nodes[i].op == PredicateOp::Greater;
    std::ostringstream out;
    out << "exact Boolean predicate DAG; nodes=" << node_count << "; leaves=" << leaves
        << "; loads=" << load_count << "; certificates=" << certificate_count
        << "; inverse_demands=" << lowering_steps;
    return out.str();
}

EncodedPredicateExecutor::EncodedPredicateExecutor(const PackedDataset& data, const PredicatePlan& plan)
    : data_(data), plan_(plan) {
    validate_plan(plan);
    const auto validate_range = [&](const Load& load) {
        if (load.channel >= data.info().channels) throw std::invalid_argument("predicate channel outside packed dataset");
        const auto range = data.info().ranges[load.channel];
        if (load.range.lo > range.min || load.range.hi < range.max)
            throw std::invalid_argument("predicate range does not cover packed dataset");
    };
    for (std::size_t i = 0; i < plan.certificate_count; ++i) validate_range(plan.certificates[i]);
    for (std::size_t i = 0; i < plan.load_count; ++i) validate_range(plan.loads[i]);
    const auto layout=sign_layout(plan);
    sign_plan_=layout.valid;
    if(sign_plan_) {
        sign_slots_.assign(layout.slots.begin(),layout.slots.begin()+plan.node_count);
        sign_arena_.resize(static_cast<std::size_t>(layout.slot_count)*sign_tile_words);
        sign_views_.resize(plan.node_count);planes_.resize(plan.load_count);
        return;
    }
    known_.resize(plan.node_count); truth_.resize(plan.node_count);
    words_.resize(static_cast<std::size_t>(plan.load_count)*32);
    planes_.resize(words_.size());
    words_loaded_.resize(plan.load_count); planes_loaded_.resize(plan.load_count);
}
std::uint64_t EncodedPredicateExecutor::required_scratch_bytes(const PredicatePlan& plan) noexcept {
    const auto layout=sign_layout(plan);
    if(layout.valid)return sizeof(EncodedPredicateExecutor)+
        std::uint64_t{plan.node_count}*(sizeof(std::uint16_t)+sizeof(SignView))+
        std::uint64_t{plan.load_count}*sizeof(std::span<const std::uint8_t>)+
        std::uint64_t{layout.slot_count}*sign_tile_words*sizeof(std::uint64_t);
    return sizeof(EncodedPredicateExecutor)+std::uint64_t{plan.node_count}*2*sizeof(std::uint64_t)+
        std::uint64_t{plan.load_count}*(32*(sizeof(std::uint64_t)+sizeof(std::span<const std::uint8_t>))+2*sizeof(std::uint32_t));
}
std::uint64_t EncodedPredicateExecutor::scratch_bytes() const noexcept {
    return sizeof(*this)+(known_.capacity()+truth_.capacity()+words_.capacity())*sizeof(std::uint64_t)+
        planes_.capacity()*sizeof(std::span<const std::uint8_t>)+
        (words_loaded_.capacity()+planes_loaded_.capacity())*sizeof(std::uint32_t)+
        sign_slots_.capacity()*sizeof(std::uint16_t)+sign_arena_.capacity()*sizeof(std::uint64_t)+
        sign_views_.capacity()*sizeof(SignView);
}
std::string EncodedPredicateExecutor::description() const {
    return std::string(sign_plan_?"encoded sign-mask operator batches with live-slot reuse; ":"encoded active-lane Boolean masks; ")+plan_.describe();
}

ExecutionStats EncodedPredicateExecutor::run_sign(std::uint64_t first_row,std::uint64_t row_count,
                                                 std::span<std::uint64_t> mask) {
    const auto rows=row_count;
    const auto end_row=first_row+row_count;
    ExecutionStats stats;stats.rows=rows;stats.output_bytes=mask.size_bytes();
    stats.peak_scratch_bytes=scratch_bytes();stats.kernel="encoded-sign-dag";
    if(!rows)return stats;
    const auto block_rows=data_.info().block_rows;
    for(std::uint64_t block=first_row/block_rows;block<=(end_row-1)/block_rows;++block) {
        const auto block_first=block*block_rows;
        const auto length=data_.block_length(block);
        const auto begin=std::max(first_row,block_first)-block_first;
        const auto end=std::min(end_row,block_first+length)-block_first;
        const auto word_begin=static_cast<std::size_t>((begin/64)*64);
        const auto byte_begin=word_begin/8;
        const auto byte_end=static_cast<std::size_t>((end+7)/8);
        for(std::uint16_t load=0;load<plan_.load_count;++load) {
            const auto plane=data_.plane_view(block,plan_.loads[load].channel,0,1);
            planes_[load]=plane;stats.requested_payload_bytes+=byte_end-byte_begin;++stats.loaded_planes;
            const auto offset=reinterpret_cast<std::uintptr_t>(plane.data()+byte_begin)%64;
            stats.estimated_cacheline_bytes+=((offset+byte_end-byte_begin+63)/64)*64;
        }
        for(std::size_t local=word_begin;local<end;local+=sign_tile_words*64) {
            const auto count=std::min<std::size_t>(sign_tile_words*64,end-local);
            const auto words=(count+63)/64;
            const auto byte_offset=local/8;
            for(std::uint16_t i=0;i<plan_.node_count;++i) {
                const auto& node=plan_.nodes[i];
                if(node.op==PredicateOp::Greater) {
                    const auto plane=planes_[node.load_index];
                    sign_views_[i]={plane.data()+byte_offset,std::min((count+7)/8,plane.size()-byte_offset)};
                    continue;
                }
                auto* destination=sign_arena_.data()+static_cast<std::size_t>(sign_slots_[i])*sign_tile_words;
                const auto a=sign_views_[node.a],b=sign_views_[node.b];
                switch(node.op) {
                case PredicateOp::False:std::fill_n(destination,words,std::uint64_t{0});break;
                case PredicateOp::True:std::fill_n(destination,words,~std::uint64_t{0});break;
                case PredicateOp::And: {
                    const auto full=std::min({words,a.size/8,b.size/8});std::size_t word=0;
                    for(;word<full;++word)destination[word]=full_sign_word(a.bytes+word*8)&full_sign_word(b.bytes+word*8);
                    for(;word<words;++word)destination[word]=partial_sign_word(a.bytes,a.size,word)&partial_sign_word(b.bytes,b.size,word);
                    break;
                }
                case PredicateOp::Or: {
                    const auto full=std::min({words,a.size/8,b.size/8});std::size_t word=0;
                    for(;word<full;++word)destination[word]=full_sign_word(a.bytes+word*8)|full_sign_word(b.bytes+word*8);
                    for(;word<words;++word)destination[word]=partial_sign_word(a.bytes,a.size,word)|partial_sign_word(b.bytes,b.size,word);
                    break;
                }
                case PredicateOp::Not: {
                    const auto full=std::min(words,a.size/8);std::size_t word=0;
                    for(;word<full;++word)destination[word]=~full_sign_word(a.bytes+word*8);
                    for(;word<words;++word)destination[word]=~partial_sign_word(a.bytes,a.size,word);
                    break;
                }
                case PredicateOp::Greater:break;
                }
                sign_views_[i]={reinterpret_cast<const std::uint8_t*>(destination),words*8};
            }
            const auto first=block_first+local;
            const auto emit_first=std::max(first,first_row);
            const auto view=sign_views_[plan_.root];
            range_detail::copy(mask,emit_first-first_row,std::span<const std::uint8_t>(view.bytes,view.size),
                               emit_first-first,first+count-emit_first);
            ++stats.bulk_resolutions;
        }
        ++stats.region_loads;
    }
    if(rows%64)mask.back()&=low_mask(static_cast<unsigned>(rows%64));
    stats.accepted=mask_popcount(mask);
    stats.resolved_by_bits[plan_.load_count?1:0]=rows;
    return stats;
}

std::uint64_t EncodedPredicateExecutor::plane_word(std::uint16_t load, unsigned bit, ExecutionStats& stats) {
    const auto flag = std::uint32_t{1} << bit;
    const auto index = static_cast<std::size_t>(load)*32+bit;
    if (words_loaded_[load] & flag) { ++stats.cache_reuses; return words_[index]; }
    if (!(planes_loaded_[load] & flag)) {
        planes_[index] = data_.plane_view(block_, plan_.loads[load].channel, bit, 1);
        planes_loaded_[load] |= flag;
        ++stats.loaded_planes;
    }
    const auto plane = planes_[index];
    const auto offset = word_*8;
    const auto size = std::min<std::size_t>(8, plane.size()-offset);
    std::uint64_t value = 0;
    if constexpr (std::endian::native == std::endian::little) {
        if (size == 8) std::memcpy(&value, plane.data()+offset, 8);
        else for (std::size_t b = 0; b < size; ++b) value |= std::uint64_t{plane[offset+b]} << (8*b);
    } else for (std::size_t b = 0; b < size; ++b) value |= std::uint64_t{plane[offset+b]} << (8*b);
    words_loaded_[load] |= flag; words_[index] = value;
    stats.requested_payload_bytes += size;
    const auto line_offset = reinterpret_cast<std::uintptr_t>(plane.data()+offset)%64;
    stats.estimated_cacheline_bytes += ((line_offset+size+63)/64)*64;
    return value;
}
std::uint64_t EncodedPredicateExecutor::compare(const PredicateNode& node, std::uint64_t active, ExecutionStats& stats) {
    std::uint64_t equal = active, greater = 0;
    const auto threshold = std::bit_cast<std::uint32_t>(node.threshold)^0x80000000U;
    for (unsigned bit = 0; bit < 32 && equal; ++bit) {
        depth_seen_[bit+1] |= equal;
        maximum_depth_=std::max(maximum_depth_,bit+1);
        const auto x = plane_word(node.load_index, bit, stats);
        if (threshold & (std::uint32_t{1} << (31-bit))) equal &= x;
        else { greater |= equal & x; equal &= ~x; }
        const auto suffix = low_mask(31-bit);
        if ((std::uint64_t{threshold} & suffix) == suffix) break;
    }
    return greater & active;
}
std::uint64_t EncodedPredicateExecutor::evaluate(PredicateNodeId id, std::uint64_t active, ExecutionStats& stats) {
    const auto missing = active & ~known_[id];
    if (!missing) return truth_[id]&active;
    const auto& node = plan_.nodes[id];
    std::uint64_t answer = 0;
    switch (node.op) {
    case PredicateOp::False: break;
    case PredicateOp::True: answer = missing; break;
    case PredicateOp::Greater: answer = compare(node, missing, stats); break;
    case PredicateOp::And:
        answer = evaluate(node.a, missing, stats);
        answer = evaluate(node.b, answer, stats); break;
    case PredicateOp::Or:
        answer = evaluate(node.a, missing, stats);
        answer |= evaluate(node.b, missing&~answer, stats); break;
    case PredicateOp::Not: answer = missing&~evaluate(node.a, missing, stats); break;
    }
    known_[id] |= missing; truth_[id] |= answer;
    return truth_[id]&active;
}
ExecutionStats EncodedPredicateExecutor::run(std::span<std::uint64_t> mask) {
    return run_range(0,data_.info().rows,mask);
}
ExecutionStats EncodedPredicateExecutor::run_range(std::uint64_t first_row,std::uint64_t row_count,
                                                  std::span<std::uint64_t> mask) {
    const auto end_row=range_detail::validate(data_.info().rows,first_row,row_count,mask);
    const auto rows=row_count;
    if(sign_plan_)return run_sign(first_row,row_count,mask);
    ExecutionStats stats; stats.rows=rows;stats.output_bytes=mask.size_bytes();
    stats.peak_scratch_bytes=scratch_bytes();stats.kernel="encoded-predicate";
    if(!rows)return stats;
    const auto block_rows=data_.info().block_rows;
    for (block_=first_row/block_rows; block_<=(end_row-1)/block_rows; ++block_) {
        std::fill(planes_loaded_.begin(), planes_loaded_.end(), 0);
        const auto block_first=block_*block_rows;
        const auto length = data_.block_length(block_);
        const auto begin=std::max(first_row,block_first)-block_first;
        const auto end=std::min(end_row,block_first+length)-block_first;
        for (word_=static_cast<std::size_t>(begin/64); word_*64<end; ++word_) {
            const auto lo=static_cast<unsigned>(std::max<std::uint64_t>(begin,word_*64)-word_*64);
            const auto hi=static_cast<unsigned>(std::min<std::uint64_t>(end,word_*64+64)-word_*64);
            count_=hi-lo;
            std::fill(known_.begin(),known_.end(),0);std::fill(truth_.begin(),truth_.end(),0);
            std::fill(words_loaded_.begin(),words_loaded_.end(),0);depth_seen_.fill(0);maximum_depth_=0;
            const auto active=low_mask(hi)&~low_mask(lo);
            const auto answer=evaluate(plan_.root,active,stats);
            set_bits(mask,block_first+word_*64+lo-first_row,answer>>lo,count_);
            auto unassigned=active;
            std::uint64_t unassigned_count=count_;
            for (unsigned bits=maximum_depth_;bits&&unassigned;--bits) {
                const auto resolved=unassigned&depth_seen_[bits];
                if(!resolved)continue;
                const auto count=resolved==unassigned?unassigned_count:static_cast<std::uint64_t>(std::popcount(resolved));
                stats.resolved_by_bits[bits]+=count;
                if(bits>4)stats.refined_rows+=count;
                unassigned&=~resolved;unassigned_count-=count;
            }
            stats.resolved_by_bits[0]+=unassigned_count;
        }
        ++stats.region_loads;
    }
    stats.accepted=mask_popcount(mask);
    return stats;
}

DensePredicateExecutor::DensePredicateExecutor(const RawDataset& data,const PredicatePlan& plan,bool allow_simd,bool inputs_already_verified)
    :data_(data),plan_(plan) {
    validate_plan(plan);
    std::array<const Load*,2*max_program_nodes> checked{};
    std::size_t checked_count=0;
    const auto validate_range=[&](const Load& load) {
        if(load.channel>=data.info().channels)throw std::invalid_argument("predicate channel outside raw dataset");
        if(inputs_already_verified)return;
        // Compiler-produced leaf contracts duplicate their certificates. Scan
        // each distinct channel/range only once, without heap bookkeeping.
        for(std::size_t i=0;i<checked_count;++i)
            if(checked[i]->channel==load.channel&&checked[i]->range==load.range)return;
        for(std::uint64_t row=0;row<data.info().rows;++row) {
            const auto value=data.value(row,load.channel);
            if(value<load.range.lo||value>load.range.hi)throw std::invalid_argument("raw value outside predicate range");
        }
        checked[checked_count++]=&load;
    };
    for(std::size_t i=0;i<plan.certificate_count;++i)validate_range(plan.certificates[i]);
    for(std::size_t i=0;i<plan.load_count;++i)validate_range(plan.loads[i]);
    known_.resize(plan.node_count);truth_.resize(plan.node_count);
#if SUFFICE_PREDICATE_X86
    use_simd_=allow_simd&&avx2_available();
#else
    (void)allow_simd;
#endif
}
std::uint64_t DensePredicateExecutor::required_scratch_bytes(const PredicatePlan& plan) noexcept {
    return sizeof(DensePredicateExecutor)+std::uint64_t{plan.node_count}*2*sizeof(std::uint64_t);
}
std::uint64_t DensePredicateExecutor::scratch_bytes() const noexcept {
    return sizeof(*this)+(known_.capacity()+truth_.capacity())*sizeof(std::uint64_t);
}
std::string DensePredicateExecutor::description() const {
    return std::string(use_simd_?"dense AVX2 Boolean predicate masks; ":"dense scalar Boolean predicate masks; ")+plan_.describe();
}
std::uint64_t DensePredicateExecutor::compare(const PredicateNode& node,std::uint64_t active,ExecutionStats& stats) {
    stats.exact_evaluations+=static_cast<std::uint64_t>(std::popcount(active));
    const auto channel=plan_.loads[node.load_index].channel;
#if SUFFICE_PREDICATE_X86
    if(use_simd_)return dense_compare_avx2(data_.bytes().data(),first_,count_,data_.info().channels,channel,node.threshold,active);
#endif
    std::uint64_t result=0;
    auto remaining=active;
    while(remaining) {
        const auto bit=static_cast<unsigned>(std::countr_zero(remaining));
        const auto flag=std::uint64_t{1}<<bit;
        if(raw_value(data_.bytes().data(),(first_+bit)*data_.info().channels+channel)>node.threshold)result|=flag;
        remaining&=remaining-1;
    }
    return result;
}
std::uint64_t DensePredicateExecutor::evaluate(PredicateNodeId id,std::uint64_t active,ExecutionStats& stats) {
    const auto missing=active&~known_[id];
    if(!missing)return truth_[id]&active;
    const auto& node=plan_.nodes[id];std::uint64_t answer=0;
    switch(node.op) {
    case PredicateOp::False:break;
    case PredicateOp::True:answer=missing;break;
    case PredicateOp::Greater:answer=compare(node,missing,stats);break;
    case PredicateOp::And:answer=evaluate(node.a,missing,stats);answer=evaluate(node.b,answer,stats);break;
    case PredicateOp::Or:answer=evaluate(node.a,missing,stats);answer|=evaluate(node.b,missing&~answer,stats);break;
    case PredicateOp::Not:answer=missing&~evaluate(node.a,missing,stats);break;
    }
    known_[id]|=missing;truth_[id]|=answer;return truth_[id]&active;
}
ExecutionStats DensePredicateExecutor::run(std::span<std::uint64_t> mask) {
    return run_range(0,data_.info().rows,mask);
}
ExecutionStats DensePredicateExecutor::run_range(std::uint64_t first_row,std::uint64_t row_count,
                                                std::span<std::uint64_t> mask) {
    const auto end_row=range_detail::validate(data_.info().rows,first_row,row_count,mask);
    const auto rows=row_count;
    ExecutionStats stats;stats.rows=rows;stats.output_bytes=mask.size_bytes();stats.peak_scratch_bytes=scratch_bytes();
    stats.kernel=use_simd_?"dense-predicate-avx2":"dense-predicate-scalar";
    stats.requested_payload_bytes=rows*plan_.load_count*4;
    for(first_=first_row;first_<end_row;first_+=count_) {
        count_=static_cast<unsigned>(std::min<std::uint64_t>(64,end_row-first_));
        std::fill(known_.begin(),known_.end(),0);std::fill(truth_.begin(),truth_.end(),0);
        set_bits(mask,first_-first_row,evaluate(plan_.root,low_mask(count_),stats),count_);
    }
    stats.accepted=mask_popcount(mask);
    const auto root_op=plan_.nodes[plan_.root].op;
    if(root_op==PredicateOp::False||root_op==PredicateOp::True) {
        stats.requested_payload_bytes=0;stats.resolved_by_bits[0]=rows;
    } else stats.resolved_by_bits[32]=rows;
    return stats;
}

} // namespace suffice
