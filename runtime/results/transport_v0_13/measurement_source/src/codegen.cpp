#include "suffice/codegen.hpp"
#include "suffice/predicate.hpp"
#include "suffice/wordcode.hpp"
#include "suffice/frontier.hpp"
#include "suffice/residual.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace suffice {
namespace {

bool valid_namespace(std::string_view name) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (name.empty() || !letter(name.front()) || name.find("__") != std::string_view::npos) return false;
    for (const char c : name) if (!letter(c) && !(c >= '0' && c <= '9') && c != '_') return false;
    constexpr std::array keywords{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
        "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
        "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "std"
    };
    for (auto keyword : keywords) if (name == keyword) return false;
    return true;
}

std::string literal(i64 value) {
    if (value == std::numeric_limits<i64>::min())
        return "(std::numeric_limits<std::int64_t>::min())";
    return "(" + std::to_string(value) + "LL)";
}
std::string node(std::uint16_t id) { return "n" + std::to_string(id); }

std::string cpp_string(std::string_view value) {
    std::string result="\"";
    for(const unsigned char c:value) {
        if(c=='\\' || c=='\"') {result+='\\';result+=static_cast<char>(c);}
        else if(c<32 || c>=127) {
            result+='\\';result+=static_cast<char>('0'+((c>>6)&7));
            result+=static_cast<char>('0'+((c>>3)&7));result+=static_cast<char>('0'+(c&7));
        } else result+=static_cast<char>(c);
    }
    result+='\"';return result;
}

bool residual_strategy(EncodedStrategy strategy) noexcept {
    return strategy==EncodedStrategy::Residual || strategy==EncodedStrategy::GuardedResidual;
}

void emit_scalar(std::ostringstream& out, const CompiledProgram& plan,
                 const PredicatePlan& predicate, bool lowered) {
    out << R"CPP(
inline bool evaluate_scalar([[maybe_unused]] const std::int32_t* values,
                            [[maybe_unused]] std::uint64_t row,
                            [[maybe_unused]] std::uint64_t rows,
                            [[maybe_unused]] std::uint32_t channels) noexcept {
)CPP";
    if (lowered) {
        for (std::uint16_t i = 0; i < predicate.node_count; ++i) {
            const auto& n = predicate.nodes[i];
            out << "    const bool " << node(i) << " = ";
            switch (n.op) {
            case PredicateOp::False: out << "false"; break;
            case PredicateOp::True: out << "true"; break;
            case PredicateOp::Greater:
                out << "values[raw_index(row, " << predicate.loads[n.load_index].channel
                    << "u, rows, channels)] > " << literal(n.threshold); break;
            case PredicateOp::And: out << node(n.a) << " & " << node(n.b); break;
            case PredicateOp::Or: out << node(n.a) << " | " << node(n.b); break;
            case PredicateOp::Not: out << "!" << node(n.a); break;
            }
            out << ";\n";
        }
        out << "    return " << node(predicate.root) << ";\n}\n";
    } else {
        const auto instructions = plan.nodes();
        for (std::uint16_t i = 0; i < instructions.size(); ++i) {
            const auto& n = instructions[i];
            out << "    const std::int64_t " << node(i) << " = ";
            switch (n.op) {
            case Op::Load: {
                const auto& load = plan.loads()[n.load_index];
                out << "values[raw_index(clamp_row(row, " << load.offset << ", rows), " << load.channel << "u, rows, channels)]";
                break;
            }
            case Op::Constant: out << literal(n.immediate); break;
            case Op::Add: out << node(n.a) << " + " << node(n.b); break;
            case Op::Sub: out << node(n.a) << " - " << node(n.b); break;
            case Op::MulConst: out << node(n.a) << " * " << literal(n.immediate); break;
            case Op::Abs: out << "(" << node(n.a) << " < 0 ? -" << node(n.a) << " : " << node(n.a) << ")"; break;
            case Op::Square: out << node(n.a) << " * " << node(n.a); break;
            case Op::Min: out << "std::min(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case Op::Max: out << "std::max(" << node(n.a) << ", " << node(n.b) << ")"; break;
            }
            out << ";\n";
        }
        out << "    return " << node(plan.output_node()) << " > " << literal(plan.threshold()) << ";\n}\n";
    }
}

void emit_vector(std::ostringstream& out, const CompiledProgram& plan,
                 const PredicatePlan& predicate, bool lowered, const std::string& attribute) {
    out << attribute << R"CPP( inline std::uint32_t evaluate_simd(
        [[maybe_unused]] const std::int32_t* values,
        [[maybe_unused]] std::uint64_t row,
        [[maybe_unused]] std::uint64_t rows,
        [[maybe_unused]] std::uint32_t channels) noexcept {
)CPP";
    if (lowered) {
        bool two_channels = predicate.load_count == 2, eight_channels = predicate.load_count == 8;
        std::array<bool,8> seen{};
        for (std::uint16_t i=0; i<predicate.load_count; ++i) {
            const auto channel=predicate.loads[i].channel;
            if (channel < seen.size()) seen[channel]=true;
            if (channel >= 2) two_channels=false;
            if (channel >= 8) eight_channels=false;
        }
        two_channels = two_channels && seen[0] && seen[1];
        for (const auto present:seen) eight_channels = eight_channels && present;
        for (std::uint16_t i=0;i<predicate.load_count;++i) out << "    __m256i input" << i << ";\n";
        if (two_channels || eight_channels) {
            out << "    if (!raw_planar && channels == " << (two_channels?2:8) << ") {\n"
                << "        const auto tile = " << (two_channels?"load_two_channels":"load_eight_channels") << "(values, row);\n";
            for(std::uint16_t i=0;i<predicate.load_count;++i)
                out << "        input" << i << " = tile.c" << predicate.loads[i].channel << ";\n";
            out << "    } else {\n";
        }
        for (std::uint16_t i=0;i<predicate.load_count;++i)
                out << "    " << (two_channels||eight_channels?"    ":"") << "input" << i << " = load8(values, row, rows, channels, " << predicate.loads[i].channel << "u);\n";
        if(two_channels||eight_channels) out << "    }\n";
        for (std::uint16_t i = 0; i < predicate.node_count; ++i) {
            const auto& n = predicate.nodes[i];
            out << "    const __m256i " << node(i) << " = ";
            switch (n.op) {
            case PredicateOp::False: out << "_mm256_setzero_si256()"; break;
            case PredicateOp::True: out << "_mm256_set1_epi32(-1)"; break;
            case PredicateOp::Greater:
                out << "_mm256_cmpgt_epi32(input" << n.load_index
                    << ", _mm256_set1_epi32(static_cast<std::int32_t>(" << literal(n.threshold) << ")))"; break;
            case PredicateOp::And: out << "_mm256_and_si256(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case PredicateOp::Or: out << "_mm256_or_si256(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case PredicateOp::Not: out << "_mm256_xor_si256(" << node(n.a) << ", _mm256_set1_epi32(-1))"; break;
            }
            out << ";\n";
        }
        out << "    return static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps("
            << node(predicate.root) << ")));\n}\n";
    } else {
        const auto instructions = plan.nodes();
        for (std::uint16_t i = 0; i < instructions.size(); ++i) {
            const auto& n = instructions[i];
            out << "    const __m256i " << node(i) << " = ";
            switch (n.op) {
            case Op::Load: {
                const auto& load = plan.loads()[n.load_index];
                out << "load4(values, row, rows, channels, " << load.channel << "u, " << load.offset << ")";
                break;
            }
            case Op::Constant: out << "_mm256_set1_epi64x(" << literal(n.immediate) << ")"; break;
            case Op::Add: out << "_mm256_add_epi64(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case Op::Sub: out << "_mm256_sub_epi64(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case Op::MulConst: out << "multiply64(" << node(n.a) << ", _mm256_set1_epi64x(" << literal(n.immediate) << "))"; break;
            case Op::Abs: out << "absolute64(" << node(n.a) << ")"; break;
            case Op::Square: out << "multiply64(" << node(n.a) << ", " << node(n.a) << ")"; break;
            case Op::Min: out << "minimum64(" << node(n.a) << ", " << node(n.b) << ")"; break;
            case Op::Max: out << "maximum64(" << node(n.a) << ", " << node(n.b) << ")"; break;
            }
            out << ";\n";
        }
        out << "    return static_cast<std::uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64("
            << node(plan.output_node()) << ", _mm256_set1_epi64x(" << literal(plan.threshold()) << ")))));\n}\n";
    }
}

// Source-level scheduling is deliberately observable and optional. Pressure mode
// uses a Sethi-Ullman tree estimate to choose a depth-first DAG traversal, keeps
// shared Boolean operations, and rematerializes immutable plane reads at their
// use sites. The host compiler still makes the final register-allocation choice.
class ForestEmitter {
public:
    struct SourceMetrics { std::uint32_t nodes{}, planes{}, live_values{}; };
    ForestEmitter(const std::vector<WordcodeNode>& nodes,
                  const std::vector<std::uint32_t>& planes,
                  std::vector<std::uint32_t> roots, EncodedSchedule schedule,
                  const std::vector<std::uint32_t>* carries=nullptr,
                  const std::vector<bool>* common_phase=nullptr,
                  bool reachable_only=false,std::size_t priority_root_count=0)
        : nodes_(nodes), planes_(planes), schedule_(schedule), carries_(carries), roots_(roots) {
        if (schedule_ == EncodedSchedule::Legacy) {
            if(reachable_only) {
                std::vector<bool> reachable(nodes_.size());
                for(auto root:roots)reachable[root]=true;
                for(std::size_t i=nodes_.size();i--;)if(reachable[i]) {
                    const auto& n=nodes_[i];
                    if(n.op==WordcodeOp::Not || n.op==WordcodeOp::And || n.op==WordcodeOp::Or)reachable[n.a]=true;
                    if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or)reachable[n.b]=true;
                }
                for(std::size_t i=0;i<nodes_.size();++i)if(reachable[i])order_.push_back(static_cast<std::uint32_t>(i));
            } else {
                order_.resize(nodes_.size());
                std::iota(order_.begin(), order_.end(), std::uint32_t{0});
            }
        } else {
            std::vector<unsigned> pressure(nodes_.size());
            for (std::size_t i=0;i<nodes_.size();++i) {
                const auto& n=nodes_[i];
                if(n.op==WordcodeOp::Plane)pressure[i]=1;
                else if(n.op==WordcodeOp::Not)pressure[i]=pressure[n.a];
                else if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or) {
                    const auto a=pressure[n.a],b=pressure[n.b];
                    pressure[i]=std::max(a,b)+(a==b);
                }
            }
            const auto less=[&](auto a,auto b){return pressure[a]>pressure[b];};
            if(schedule_==EncodedSchedule::BoundsFirst && priority_root_count) {
                // Traverse the complete bound-root closure before consulting any
                // carry root. Sorting/partitioning a carry-driven order later
                // would not provide this scheduling property.
                const auto split=roots.begin()+static_cast<std::ptrdiff_t>(std::min(priority_root_count,roots.size()));
                std::stable_sort(roots.begin(),split,less);
                std::stable_sort(split,roots.end(),less);
            } else std::stable_sort(roots.begin(),roots.end(),less);
            std::vector<bool> emitted(nodes_.size());
            std::vector<std::pair<std::uint32_t,bool>> stack;
            for(const auto root:roots) {
                stack.emplace_back(root,false);
                while(!stack.empty()) {
                    const auto [id,visited]=stack.back();stack.pop_back();
                    if(emitted[id])continue;
                    const auto& n=nodes_[id];
                    const bool binary=n.op==WordcodeOp::And || n.op==WordcodeOp::Or;
                    if(n.op!=WordcodeOp::Not && !binary) { emitted[id]=true;continue; }
                    if(visited) { emitted[id]=true;order_.push_back(id);continue; }
                    stack.emplace_back(id,true);
                    if(binary) {
                        auto first=n.a,second=n.b;
                        if(pressure[first]<pressure[second])std::swap(first,second);
                        stack.emplace_back(second,false);stack.emplace_back(first,false);
                    } else stack.emplace_back(n.a,false);
                }
            }
        }
        // A dependency-closed first phase can be emitted before a branch while
        // retaining topological order within both phases. Its roots are the
        // must/may bounds; the remaining carry-only work belongs to repair.
        if(common_phase)std::stable_partition(order_.begin(),order_.end(),
            [&](auto id){return (*common_phase)[id];});
        // This model counts live emitted vector values, excluding constants and
        // transient operands. It is not a hardware-register or stack bound.
        std::vector<std::uint32_t> uses(nodes_.size());
        for(const auto id:order_) {
            const auto& n=nodes_[id];
            if(n.op==WordcodeOp::Not || n.op==WordcodeOp::And || n.op==WordcodeOp::Or)++uses[n.a];
            if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or)++uses[n.b];
        }
        for(const auto root:roots)++uses[root];
        std::vector<bool> live(nodes_.size());
        std::uint32_t current=0;
        for(const auto id:order_) {
            const auto& n=nodes_[id];
            const auto release=[&](std::uint32_t input) {
                if(uses[input] && --uses[input]==0 && live[input]) {live[input]=false;--current;}
            };
            if(n.op==WordcodeOp::Not || n.op==WordcodeOp::And || n.op==WordcodeOp::Or)release(n.a);
            if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or)release(n.b);
            if(n.op!=WordcodeOp::False && n.op!=WordcodeOp::True && uses[id]) {
                live[id]=true;++current;peak_=std::max(peak_,current);
            }
        }
    }

    std::uint32_t live_values() const noexcept {return peak_;}

    // Model the statements and root expressions actually emitted for a source
    // fragment. Nodes include inlined leaves and inherited named inputs; planes
    // count distinct reads in this fragment. For a guarded common phase, keep
    // named results needed by the deferred phase live at the phase boundary.
    SourceMetrics source_metrics(const std::vector<std::uint32_t>& roots,
                                 const std::vector<bool>* phase=nullptr,bool selected=true,
                                 bool retain_deferred=false) const {
        std::vector<bool> emitted(nodes_.size()),source(nodes_.size()),live(nodes_.size());
        std::vector<std::uint32_t> uses(nodes_.size());
        std::vector<std::uint32_t> read_planes;
        for(const auto id:order_)if(!phase || (*phase)[id]==selected)emitted[id]=true;
        const auto inlined=[&](std::uint32_t id) {
            const auto op=nodes_[id].op;
            return schedule_!=EncodedSchedule::Legacy &&
                (op==WordcodeOp::False || op==WordcodeOp::True || op==WordcodeOp::Plane);
        };
        const auto read_plane=[&](std::uint32_t id) {
            if(nodes_[id].op==WordcodeOp::Plane && (!carries_ || (*carries_)[id]==UINT32_MAX))
                read_planes.push_back(planes_[id]);
        };
        const auto value_use=[&](std::uint32_t id) {
            source[id]=true;
            if(inlined(id))read_plane(id);else ++uses[id];
        };
        const auto children=[&](std::uint32_t id,const auto& visit) {
            if(carries_ && (*carries_)[id]!=UINT32_MAX)return;
            const auto& n=nodes_[id];
            if(n.op==WordcodeOp::Not || n.op==WordcodeOp::And || n.op==WordcodeOp::Or)visit(n.a);
            if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or)visit(n.b);
        };
        for(const auto id:order_)if(emitted[id]) {
            source[id]=true;read_plane(id);children(id,value_use);
        }
        for(const auto root:roots)value_use(root);
        if(retain_deferred && phase) {
            const auto retain=[&](std::uint32_t id) {if(emitted[id] && !inlined(id))++uses[id];};
            for(const auto id:order_)if(!emitted[id])children(id,retain);
            for(const auto root:roots_)retain(root);
        }
        std::uint32_t current=0,peak=0;
        for(std::size_t i=0;i<nodes_.size();++i)
            if(uses[i] && !emitted[i] && nodes_[i].op!=WordcodeOp::False && nodes_[i].op!=WordcodeOp::True)
                {live[i]=true;++current;}
        peak=current;
        for(const auto id:order_)if(emitted[id]) {
            children(id,[&](std::uint32_t input) {
                if(!inlined(input) && uses[input] && --uses[input]==0 && live[input]) {live[input]=false;--current;}
            });
            if(nodes_[id].op!=WordcodeOp::False && nodes_[id].op!=WordcodeOp::True && uses[id])
                {live[id]=true;++current;peak=std::max(peak,current);}
        }
        std::sort(read_planes.begin(),read_planes.end());
        read_planes.erase(std::unique(read_planes.begin(),read_planes.end()),read_planes.end());
        return {static_cast<std::uint32_t>(std::count(source.begin(),source.end(),true)),
                static_cast<std::uint32_t>(read_planes.size()),peak};
    }

    std::string value(std::uint32_t id,const std::string& tag,bool vector,bool complete) const {
        if(schedule_!=EncodedSchedule::Legacy) {
            const auto op=nodes_[id].op;
            if(op==WordcodeOp::False || op==WordcodeOp::True || op==WordcodeOp::Plane)
                return expression(id,tag,vector,complete);
        }
        return tag+std::to_string(id);
    }

    void emit(std::ostringstream& out,const std::string& indent,const std::string& tag,
              bool vector,bool complete,const std::vector<bool>* phase=nullptr,
              bool selected=true) const {
        for(const auto id:order_)if(!phase || (*phase)[id]==selected)
            out<<indent<<"const "<<(vector?"__m256i":"std::uint64_t")<<" "<<tag<<id<<" = "
               <<expression(id,tag,vector,complete)<<";\n";
    }

private:
    const std::vector<WordcodeNode>& nodes_;
    const std::vector<std::uint32_t>& planes_;
    EncodedSchedule schedule_;
    const std::vector<std::uint32_t>* carries_{};
    std::vector<std::uint32_t> roots_;
    std::vector<std::uint32_t> order_;
    std::uint32_t peak_{};
    std::string expression(std::uint32_t id,const std::string& tag,bool vector,bool complete) const {
        if(carries_ && (*carries_)[id]!=UINT32_MAX)
            return "evidence.carries["+std::to_string((*carries_)[id])+"]";
        const auto& n=nodes_[id];
        switch(n.op) {
        case WordcodeOp::False:return vector?"zero":"std::uint64_t{0}";
        case WordcodeOp::True:return vector?"ones":"~std::uint64_t{0}";
        case WordcodeOp::Plane: {
            const auto index=std::to_string(planes_[id]);
            if(vector) {
                const auto address=schedule_==EncodedSchedule::Legacy?"p"+index+"+byte":
                    "planes["+index+"].data()+offset/8+byte";
                return "_mm256_loadu_si256(reinterpret_cast<const __m256i*>("+address+"))";
            }
            return complete?"read_complete_word(planes["+index+"].data()+offset/8)":
                            "read_plane_bits(planes["+index+"],offset,count)";
        }
        case WordcodeOp::Not: {
            const auto a=value(n.a,tag,vector,complete);
            return vector?"_mm256_xor_si256("+a+",ones)":"(~("+a+"))";
        }
        case WordcodeOp::And:case WordcodeOp::Or: {
            const auto a=value(n.a,tag,vector,complete),b=value(n.b,tag,vector,complete);
            if(vector)return std::string(n.op==WordcodeOp::And?"_mm256_and_si256(":"_mm256_or_si256(")+a+","+b+")";
            return "("+a+(n.op==WordcodeOp::And?" & ":" | ")+b+")";
        }
        }
        throw std::logic_error("invalid wordcode operation in native emitter");
    }
};

void emit_encoded(std::ostringstream& out,const WordcodePlan& plan,const FrontierPlan& frontier_plan,
                  const ResidualPlan& residual_plan,
                  const CodegenOptions& options,const std::string& macro,const std::string& attribute,
                  GeneratedKernel& metadata) {
    const bool residual=residual_strategy(options.encoded_strategy);
    const bool guarded_residual=options.encoded_strategy==EncodedStrategy::GuardedResidual;
    const bool guarded_carries=residual && options.encoded_carry_placement==EncodedCarryPlacement::Guarded;
    const bool recompute_carries=residual && options.encoded_carry_placement==EncodedCarryPlacement::Recompute;
    const bool narrow_repair=options.encoded_repair_policy!=EncodedRepairPolicy::Batch256;
    const bool frontier=options.encoded_strategy!=EncodedStrategy::Eager;
    const auto& prefix_plan=residual?residual_plan.prefix:frontier_plan.prefix;
    const bool complete=plan.complete && (!frontier || (residual?residual_plan.complete:frontier_plan.complete));
    std::vector<WordcodeNode> residual_nodes;
    std::vector<std::uint32_t> carry_indices;
    if(complete && residual) {
        validate_residual(residual_plan);
        for(const auto& n:residual_plan.suffix.nodes) {
            WordcodeOp op{};
            switch(n.op) {
            case ResidualOp::False:op=WordcodeOp::False;break;
            case ResidualOp::True:op=WordcodeOp::True;break;
            case ResidualOp::Plane:case ResidualOp::Carry:op=WordcodeOp::Plane;break;
            case ResidualOp::And:op=WordcodeOp::And;break;
            case ResidualOp::Or:op=WordcodeOp::Or;break;
            case ResidualOp::Not:op=WordcodeOp::Not;break;
            }
            residual_nodes.push_back({op,n.a,n.b,n.load_index,n.bit,n.slot});
            carry_indices.push_back(n.op==ResidualOp::Carry?n.carry_index:UINT32_MAX);
        }
    }
    std::vector<std::pair<std::uint32_t,unsigned>> plane_contracts;
    std::map<std::pair<std::uint32_t,unsigned>,std::uint32_t> plane_map;
    const auto map_planes=[&](const auto& nodes,const auto& loads,const std::vector<std::uint32_t>* carries=nullptr) {
        std::vector<std::uint32_t> result(nodes.size());
        for(std::size_t i=0;i<nodes.size();++i)if(nodes[i].op==WordcodeOp::Plane && (!carries || (*carries)[i]==UINT32_MAX)) {
            const auto key=std::pair{loads[nodes[i].load_index].channel,unsigned(nodes[i].bit)};
            auto [it,inserted]=plane_map.emplace(key,static_cast<std::uint32_t>(plane_contracts.size()));
            if(inserted)plane_contracts.push_back(key);
            result[i]=it->second;
        }
        return result;
    };
    const auto plane_indices=complete&&!residual?map_planes(plan.nodes,plan.loads):std::vector<std::uint32_t>(plan.nodes.size());
    const auto prefix_indices=complete&&frontier?map_planes(prefix_plan.nodes,prefix_plan.loads):std::vector<std::uint32_t>{};
    const auto residual_indices=complete&&residual?map_planes(residual_nodes,residual_plan.prefix.loads,&carry_indices):std::vector<std::uint32_t>{};
    const auto plane_count=plane_contracts.size();
    // Keep the original consumer slot order. The separate map is ordered by
    // physical channel/bit, so each maximal consecutive run can be requested
    // once without asking the Dataset for unreferenced gap planes.
    struct BindingGroup { std::uint32_t channel,first_bit,count_bits,first_slot; };
    std::vector<BindingGroup> binding_groups;
    std::vector<std::uint32_t> binding_slots;
    for(const auto& [key,slot]:plane_map) {
        const auto [channel,bit]=key;
        if(binding_groups.empty() || binding_groups.back().channel!=channel ||
           binding_groups.back().first_bit+binding_groups.back().count_bits!=bit)
            binding_groups.push_back({channel,bit,0,static_cast<std::uint32_t>(binding_slots.size())});
        ++binding_groups.back().count_bits;
        binding_slots.push_back(slot);
    }
    const bool grouped_binding=options.encoded_plane_binding==EncodedPlaneBinding::Grouped;
    metadata.encoded_grouped_binding_emitted=complete && grouped_binding;
    metadata.encoded_plane_count=static_cast<std::uint32_t>(plane_count);
    metadata.encoded_binding_groups=static_cast<std::uint32_t>(binding_groups.size());
    metadata.encoded_binding_calls_per_block=static_cast<std::uint32_t>(grouped_binding?binding_groups.size():plane_count);
    const bool vector_encoded=complete && options.enable_avx2 && plane_count!=0;
    metadata.encoded_narrow_repair_emitted=vector_encoded && narrow_repair;
    const auto repair_policy_name=options.encoded_repair_policy==EncodedRepairPolicy::Word64?"word64":
        options.encoded_repair_policy==EncodedRepairPolicy::Hybrid?"hybrid":"batch256";
    const auto schedule_name=options.encoded_schedule==EncodedSchedule::Legacy?"legacy":
        options.encoded_schedule==EncodedSchedule::BoundsFirst?"bounds-first":"pressure";
    out<<"inline constexpr const char* encoded_strategy = \""<<(guarded_residual?"guarded-residual":residual?"residual":frontier?"frontier":"eager")<<"\";\n";
    out<<"inline constexpr const char* encoded_schedule = \""<<schedule_name<<"\";\n";
    out<<"inline constexpr const char* encoded_carry_placement = \""<<(recompute_carries?"recompute":guarded_carries?"guarded":"eager")<<"\";\n";
    out<<"inline constexpr const char* encoded_plane_binding = \""<<(grouped_binding?"grouped":"per-plane")<<"\";\n";
    out<<"inline constexpr const char* encoded_repair_policy = \""<<repair_policy_name<<"\";\n";
    out<<"inline constexpr bool encoded_narrow_repair_emitted = "<<(metadata.encoded_narrow_repair_emitted?"true":"false")<<";\n";
    out<<"inline constexpr unsigned encoded_word_repair_max_dirty = "
       <<(options.encoded_repair_policy==EncodedRepairPolicy::Word64?4:options.encoded_repair_policy==EncodedRepairPolicy::Hybrid?1:0)<<";\n";
    out<<"inline constexpr bool encoded_grouped_binding_emitted = "<<(complete&&grouped_binding?"true":"false")<<";\n";
    out<<"inline constexpr std::size_t encoded_binding_groups = "<<binding_groups.size()<<";\n";
    out<<"inline constexpr std::size_t encoded_binding_calls_per_block = "<<metadata.encoded_binding_calls_per_block<<";\n";
    out<<"inline constexpr const char* encoded_residual_scope = \""<<(complete&&residual?(residual_plan.scope==ResidualScope::AllLanes?"all-lanes":"unresolved-lanes"):"none")<<"\";\n";
    out<<"inline constexpr unsigned encoded_prefix_bits = "<<(complete&&frontier?options.encoded_prefix_bits:0)<<";\n";
    out<<"inline constexpr bool encoded_frontier_emitted = "<<(complete&&frontier&&!residual?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_residual_emitted = "<<(complete&&residual?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_guarded_residual_emitted = "<<(complete&&guarded_residual?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_guarded_carry_emitted = "<<(complete&&guarded_carries?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_recompute_carry_emitted = "<<(complete&&recompute_carries?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_residual_exact_all_lanes = "<<(complete&&residual&&residual_plan.scope==ResidualScope::AllLanes?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_emitted = "<<(complete?"true":"false")<<";\n";
    out<<"inline constexpr bool encoded_avx2_emitted = "<<(vector_encoded?"true":"false")<<";\n";
    out<<"inline constexpr unsigned encoded_simd_rows = "<<(vector_encoded?256:0)<<";\n";
    out<<"inline constexpr std::size_t wordcode_nodes = "<<(complete?plan.nodes.size():0)<<";\n";
    out<<"inline constexpr std::size_t encoded_plane_count = "<<plane_count<<";\n";
    out<<"inline constexpr std::size_t encoded_prefix_nodes = "<<(complete&&frontier?prefix_plan.nodes.size():0)<<";\n";
    out<<"inline constexpr std::size_t encoded_prefix_planes = "<<(complete&&frontier?prefix_plan.plane_count:0)<<";\n";
    out<<"inline constexpr std::size_t encoded_residual_nodes = "<<(complete&&residual?residual_nodes.size():0)<<";\n";
    out<<"inline constexpr std::size_t encoded_residual_planes = "<<(complete&&residual?residual_plan.suffix.plane_count:0)<<";\n";
    out<<"inline constexpr std::size_t encoded_residual_carries = "<<(complete&&residual?residual_plan.carry_roots.size():0)<<";\n";
    out<<"inline constexpr std::size_t encoded_residual_initial_carries = "<<(complete&&residual?(guarded_residual?residual_plan.proof.initial_carries:residual_plan.carry_roots.size()):0)<<";\n";
    out<<"inline constexpr std::uint32_t encoded_proof_attempts = "<<(complete&&guarded_residual?residual_plan.proof.attempts:0)<<";\n";
    out<<"inline constexpr std::uint32_t encoded_proof_accepted_rewrites = "<<(complete&&guarded_residual?residual_plan.proof.accepted_rewrites:0)<<";\n";
    out<<"inline constexpr std::uint32_t encoded_proof_bdd_nodes = "<<(complete&&guarded_residual?residual_plan.proof.bdd_nodes:0)<<";\n";
    out<<"inline constexpr std::uint32_t encoded_proof_graph_budget_rejections = "<<(complete&&guarded_residual?residual_plan.proof.graph_budget_rejections:0)<<";\n";
    out<<"inline constexpr std::uint64_t encoded_proof_steps = "<<(complete&&guarded_residual?residual_plan.proof.proof_steps:0)<<"ULL;\n";
    out<<"inline constexpr bool encoded_proof_exhausted = "<<(complete&&guarded_residual&&residual_plan.proof.exhausted?"true":"false")<<";\n";
    out<<"inline constexpr const char* encoded_proof_reason = "<<cpp_string(complete&&guarded_residual?residual_plan.proof.reason:"")<<";\n";
    if(residual)out<<"// If encoded_residual_exact_all_lanes is false, a suffix helper agrees\n"
                     "// with the exact predicate only where evidence.may & ~evidence.must is set.\n"
                     "// Callers must merge it under that care mask with evidence.must.\n";
    out<<"struct PlaneContract { std::uint32_t channel, bit; };\n";
    out<<"inline constexpr std::array<PlaneContract,"<<plane_count<<"> plane_contracts{{\n";
    for(const auto& [channel,bit]:plane_contracts)out<<"    {"<<channel<<"u,"<<bit<<"u},\n";
    out<<"}};\n";
    out<<"struct PlaneBindingGroup { std::uint32_t channel, first_bit, count_bits, first_slot; };\n";
    out<<"inline constexpr std::array<PlaneBindingGroup,"<<binding_groups.size()<<"> plane_binding_groups{{\n";
    for(const auto& group:binding_groups)
        out<<"    {"<<group.channel<<"u,"<<group.first_bit<<"u,"<<group.count_bits<<"u,"<<group.first_slot<<"u},\n";
    out<<"}};\ninline constexpr std::array<std::uint32_t,"<<binding_slots.size()<<"> plane_binding_slots{{";
    for(const auto slot:binding_slots)out<<slot<<"u,";
    out<<"}};\n";
    out<<R"CPP(template<std::size_t P,std::size_t G,std::size_t S>
constexpr bool valid_encoded_binding_plan(const std::array<PlaneContract,P>& contracts,
                                         const std::array<PlaneBindingGroup,G>& groups,
                                         const std::array<std::uint32_t,S>& slots) noexcept {
    if constexpr(S!=P)return false;
    std::array<bool,P> seen{};
    std::size_t covered=0;
    for(const auto& group:groups) {
        if(!group.count_bits || group.first_bit>=32 || group.count_bits>32-group.first_bit ||
           group.first_slot>S || group.count_bits>S-group.first_slot)return false;
        for(std::uint32_t bit=0;bit<group.count_bits;++bit) {
            const auto slot=slots[group.first_slot+bit];
            if(slot>=P || seen[slot])return false;
            const auto& contract=contracts[slot];
            if(contract.channel!=group.channel || contract.bit!=group.first_bit+bit)return false;
            seen[slot]=true;++covered;
        }
    }
    return covered==P;
}
inline constexpr bool encoded_binding_plan_valid =
    valid_encoded_binding_plan(plane_contracts,plane_binding_groups,plane_binding_slots);
static_assert(encoded_binding_plan_valid,"generated encoded plane binding map is invalid");
using EncodedPlaneViews = std::array<std::span<const std::uint8_t>,encoded_plane_count>;

// Binds borrowed views only; it neither verifies payload nor source certificates.
// The Dataset must obey the same immutable layout contract as PackedDataset.
// Grouped mode additionally requires ascending consecutive bit planes, each with stride
// ceil(physical_length/8). Original consumer slot indices remain unchanged.
// On failure the caller's view array may be partially updated; do not consume it.
template<class Dataset>
inline std::uint32_t bind_encoded_planes(const Dataset& data, std::uint64_t block,
                                         EncodedPlaneViews& planes) {
    if constexpr(!encoded_emitted)
        throw std::invalid_argument("no complete encoded lowering was emitted");
    const auto& info=data.info();
    const auto block_rows=info.block_rows;
    if(block_rows<32 || block_rows>65536 || (block_rows&(block_rows-1)))
        throw std::invalid_argument("generated encoded binding dimensions are invalid");
    const auto blocks=info.rows/block_rows+(info.rows%block_rows!=0);
    if(block>=blocks)throw std::out_of_range("generated encoded binding block index");
    const auto physical_length=data.block_length(block);
    const auto expected=std::min<std::uint64_t>(block_rows,info.rows-block*block_rows);
    if(physical_length!=expected)
        throw std::invalid_argument("generated encoded block length violates layout contract");
    const auto stride=(std::size_t(physical_length)+7)/8;
)CPP";
    if(grouped_binding)out<<R"CPP(    for(const auto& group:plane_binding_groups) {
        const auto bytes=data.plane_view(block,group.channel,group.first_bit,group.count_bits);
        const auto needed=std::size_t(group.count_bits)*stride;
        if(bytes.size()<needed)
            throw std::invalid_argument("generated encoded grouped plane view is truncated");
        for(std::uint32_t bit=0;bit<group.count_bits;++bit)
            planes[plane_binding_slots[group.first_slot+bit]]=bytes.subspan(std::size_t(bit)*stride,stride);
    }
)CPP";
    else out<<R"CPP(    for(std::size_t i=0;i<plane_contracts.size();++i) {
        planes[i]=data.plane_view(block,plane_contracts[i].channel,plane_contracts[i].bit,1);
        if(planes[i].size()<stride)
            throw std::invalid_argument("generated encoded plane view is truncated");
    }
)CPP";
    out<<R"CPP(    return static_cast<std::uint32_t>(physical_length);
}
)CPP";
    if(complete) {
        ForestEmitter exact(plan.nodes,plane_indices,{plan.root},options.encoded_schedule);
        std::vector<std::uint32_t> prefix_roots=frontier?
            std::vector<std::uint32_t>{prefix_plan.must_root,prefix_plan.may_root}:std::vector<std::uint32_t>{};
        if(residual)prefix_roots.insert(prefix_roots.end(),residual_plan.carry_roots.begin(),residual_plan.carry_roots.end());
        std::vector<bool> bound_nodes(prefix_plan.nodes.size());
        if(frontier) {
            bound_nodes[prefix_plan.must_root]=bound_nodes[prefix_plan.may_root]=true;
            for(std::size_t i=prefix_plan.nodes.size();i--;)if(bound_nodes[i]) {
                const auto& n=prefix_plan.nodes[i];
                if(n.op==WordcodeOp::Not || n.op==WordcodeOp::And || n.op==WordcodeOp::Or)bound_nodes[n.a]=true;
                if(n.op==WordcodeOp::And || n.op==WordcodeOp::Or)bound_nodes[n.b]=true;
            }
        }
        ForestEmitter prefix(prefix_plan.nodes,prefix_indices,prefix_roots,options.encoded_schedule,
                             nullptr,guarded_carries?&bound_nodes:nullptr,false,frontier?2:0);
        const std::vector<std::uint32_t> bound_roots=frontier?
            std::vector<std::uint32_t>{prefix_plan.must_root,prefix_plan.may_root}:std::vector<std::uint32_t>{};
        ForestEmitter bounds(prefix_plan.nodes,prefix_indices,bound_roots,options.encoded_schedule,
                             nullptr,nullptr,true,frontier?2:0);
        ForestEmitter carry(prefix_plan.nodes,prefix_indices,residual_plan.carry_roots,options.encoded_schedule,
                            nullptr,nullptr,true);
        ForestEmitter suffix(residual_nodes,residual_indices,
            residual?std::vector<std::uint32_t>{residual_plan.suffix.root}:std::vector<std::uint32_t>{},options.encoded_schedule,&carry_indices);
        const auto common_metrics=!frontier?exact.source_metrics({plan.root}):
            recompute_carries?bounds.source_metrics(bound_roots):
            guarded_carries?prefix.source_metrics(bound_roots,&bound_nodes,true,true):prefix.source_metrics(prefix_roots);
        const auto cold_metrics=recompute_carries?carry.source_metrics(residual_plan.carry_roots):
            guarded_carries?prefix.source_metrics(residual_plan.carry_roots,&bound_nodes,false):ForestEmitter::SourceMetrics{};
        metadata.encoded_common_nodes=common_metrics.nodes;
        metadata.encoded_common_planes=common_metrics.planes;
        metadata.encoded_common_live_values=common_metrics.live_values;
        metadata.encoded_cold_prefix_nodes=cold_metrics.nodes;
        metadata.encoded_cold_prefix_planes=cold_metrics.planes;
        metadata.encoded_cold_prefix_live_values=cold_metrics.live_values;
        out<<"inline constexpr std::size_t encoded_full_live_values = "<<exact.live_values()<<";\n";
        out<<"inline constexpr std::size_t encoded_prefix_live_values = "<<prefix.live_values()<<";\n";
        out<<"inline constexpr std::size_t encoded_residual_live_values = "<<suffix.live_values()<<";\n";
        out<<R"CPP(
inline std::uint64_t read_complete_word(const std::uint8_t* bytes) noexcept {
    std::uint64_t value=0;
    if constexpr(std::endian::native==std::endian::little)std::memcpy(&value,bytes,8);
    else for(unsigned i=0;i<8;++i)value|=std::uint64_t(bytes[i])<<(i*8);
    return value;
}
inline std::uint64_t read_plane_bits(std::span<const std::uint8_t> plane,
                                    std::uint32_t offset,unsigned count) noexcept {
    const auto byte=static_cast<std::size_t>(offset/8);
    const auto shift=offset%8;
    const auto available=std::min<std::size_t>(8,plane.size()-byte);
    std::uint64_t value=0;
    if constexpr(std::endian::native==std::endian::little)std::memcpy(&value,plane.data()+byte,available);
    else for(std::size_t i=0;i<available;++i)value|=std::uint64_t(plane[byte+i])<<(i*8);
    value>>=shift;
    if(shift && count>64-shift && byte+8<plane.size())value|=std::uint64_t(plane[byte+8])<<(64-shift);
    return value&low_mask(count);
}
)CPP";
        const auto emit_scalar_forest=[&](const ForestEmitter& forest,const std::string& tag,
                                         std::uint32_t first,std::uint32_t second,bool bounds) {
            for(const bool whole:{true,false}) {
                if(whole)out<<"    if(count==64 && offset%8==0) {\n";
                const auto indent=whole?"        ":"    ";
                forest.emit(out,indent,tag,false,whole);
                out<<indent<<"return ";
                if(bounds)out<<"{";
                out<<forest.value(first,tag,false,whole)<<(whole?"":" & low_mask(count)");
                if(bounds) {
                    out<<", "<<forest.value(second,tag,false,whole)<<(whole?"":" & low_mask(count)");
                    if(residual) {
                        out<<", {";
                        for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                            if(i)out<<",";
                            out<<forest.value(residual_plan.carry_roots[i],tag,false,whole)<<(whole?"":" & low_mask(count)");
                        }
                        out<<"}";
                    }
                    out<<"}";
                }
                out<<";\n";
                if(whole)out<<"    }\n";
            }
        };
        const auto scalar_signature=[&](const std::string& name,const std::string& type,bool carry_parameter=false,
                                        bool noinline=false) {
            if(noinline)out<<macro<<"_NOINLINE ";
            out<<"inline "<<type<<" "<<name<<"(\n"
                  "        [[maybe_unused]] const std::array<std::span<const std::uint8_t>,encoded_plane_count>& planes,\n"
                  "        [[maybe_unused]] std::uint32_t offset,unsigned count";
            if(carry_parameter)out<<", [[maybe_unused]] const EncodedPrefix& evidence";
            out<<") noexcept {\n";
        };
        if(frontier) {
            out<<"struct EncodedPrefix { std::uint64_t must, may;";
            if(residual)out<<" std::array<std::uint64_t,encoded_residual_carries> carries;";
            out<<" };\n";
            scalar_signature("evaluate_encoded_prefix_unchecked","EncodedPrefix");
            emit_scalar_forest(prefix,"b",prefix_plan.must_root,prefix_plan.may_root,true);
            out<<"}\n";
            scalar_signature(residual?"evaluate_encoded_residual_unchecked":"evaluate_encoded_exact_unchecked","std::uint64_t",residual);
            emit_scalar_forest(residual?suffix:exact,"w",residual?residual_plan.suffix.root:plan.root,0,false);out<<"}\n";
            if(recompute_carries) {
                out<<"// Cold repair may reread prefix planes. The suffix helper itself remains suffix-only.\n";
                scalar_signature("evaluate_encoded_repair_unchecked","std::uint64_t",false,true);
                for(const bool whole:{true,false}) {
                    if(whole)out<<"    if(count==64 && offset%8==0) {\n";
                    const std::string indent=whole?"        ":"    ";
                    carry.emit(out,indent,"c",false,whole);
                    out<<indent<<"const EncodedPrefix evidence{0,0,{";
                    for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                        if(i)out<<",";
                        out<<carry.value(residual_plan.carry_roots[i],"c",false,whole)
                           <<(whole?"":" & low_mask(count)");
                    }
                    out<<"}};\n"<<indent<<"return evaluate_encoded_residual_unchecked(planes,offset,count,evidence);\n";
                    if(whole)out<<"    }\n";
                }
                out<<"}\n";
            }
            scalar_signature("evaluate_encoded_unchecked","std::uint64_t");
            if(guarded_carries || recompute_carries) {
                // The standalone prefix helper is diagnostic/API support. The
                // production outer path keeps bounds in local values and only
                // constructs the carry aggregate after uncertainty is known.
                for(const bool whole:{true,false}) {
                    if(whole)out<<"    if(count==64 && offset%8==0) {\n";
                    const std::string indent=whole?"        ":"    ";
                    const auto& common=recompute_carries?bounds:prefix;
                    common.emit(out,indent,"b",false,whole,recompute_carries?nullptr:&bound_nodes,true);
                    out<<indent<<"const auto must="<<common.value(prefix_plan.must_root,"b",false,whole)
                       <<(whole?"":" & low_mask(count)")<<";\n";
                    out<<indent<<"const auto may="<<common.value(prefix_plan.may_root,"b",false,whole)
                       <<(whole?"":" & low_mask(count)")<<";\n";
                    out<<indent<<"const auto unresolved=may^must;\n"
                       <<indent<<"if(!unresolved)return must;\n";
                    if(recompute_carries) {
                        out<<indent<<"return must|(unresolved&evaluate_encoded_repair_unchecked(planes,offset,count));\n";
                    } else {
                        prefix.emit(out,indent,"b",false,whole,&bound_nodes,false);
                        out<<indent<<"const EncodedPrefix evidence{must,may,{";
                        for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                            if(i)out<<",";
                            out<<prefix.value(residual_plan.carry_roots[i],"b",false,whole)
                               <<(whole?"":" & low_mask(count)");
                        }
                        out<<"}};\n"<<indent<<"return must|(unresolved&evaluate_encoded_residual_unchecked(planes,offset,count,evidence));\n";
                    }
                    if(whole)out<<"    }\n";
                }
                out<<"}\n";
            } else {
                out<<R"CPP(    const auto bounds=evaluate_encoded_prefix_unchecked(planes,offset,count);
    const auto unresolved=bounds.may^bounds.must;
    if(!unresolved)return bounds.must;
)CPP";
                out<<"    return bounds.must|(unresolved&"<<(residual?"evaluate_encoded_residual_unchecked(planes,offset,count,bounds)":
                    "evaluate_encoded_exact_unchecked(planes,offset,count)")<<");\n}\n";
            }
        } else {
            scalar_signature("evaluate_encoded_unchecked","std::uint64_t");
            emit_scalar_forest(exact,"w",plan.root,0,false);out<<"}\n";
        }
        if(vector_encoded) {
            out<<"#if "<<macro<<"_X86\n";
            const auto pointer_bases=[&](const std::string& indent,bool suffix_only=false) {
                if(options.encoded_schedule==EncodedSchedule::Legacy)
                    for(std::size_t i=0;i<plane_count;++i)
                        if(!suffix_only || plane_contracts[i].second>=options.encoded_prefix_bits)
                            out<<indent<<"[[maybe_unused]] const auto* p"<<i<<"=planes["<<i<<"].data()+offset/8;\n";
            };
            if(frontier) {
                out<<"struct EncodedPrefix256 { __m256i must, may;";
                if(residual)out<<" std::array<__m256i,encoded_residual_carries> carries;";
                out<<" };\n";
                out<<attribute<<" inline EncodedPrefix256 evaluate_encoded_prefix_avx2_unchecked(\n"
                     "        [[maybe_unused]] const std::array<std::span<const std::uint8_t>,encoded_plane_count>& planes,\n"
                     "        [[maybe_unused]] std::uint32_t offset) noexcept {\n"
                     "    [[maybe_unused]] const auto zero=_mm256_setzero_si256();\n"
                     "    [[maybe_unused]] const auto ones=_mm256_set1_epi64x(-1);\n"
                     "    [[maybe_unused]] const std::size_t byte=0;\n";
                pointer_bases("    ");prefix.emit(out,"    ","b",true,true);
                out<<"    return {"<<prefix.value(prefix_plan.must_root,"b",true,true)<<","
                   <<prefix.value(prefix_plan.may_root,"b",true,true);
                if(residual) {
                    out<<", {";
                    for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                        if(i)out<<",";
                        out<<prefix.value(residual_plan.carry_roots[i],"b",true,true);
                    }
                    out<<"}";
                }
                out<<"};\n}\n";
                // Keep cold exact work out of the prefix loop's register state.
                // C++ inline provides ODR safety; NOINLINE controls optimization.
                out<<attribute<<" "<<macro<<"_NOINLINE inline __m256i "
                     <<(residual?"evaluate_encoded_residual_avx2_unchecked":"evaluate_encoded_exact_avx2_unchecked")<<"(\n"
                     "        [[maybe_unused]] const std::array<std::span<const std::uint8_t>,encoded_plane_count>& planes,\n"
                     "        [[maybe_unused]] std::uint32_t offset";
                if(residual)out<<", [[maybe_unused]] const EncodedPrefix256& evidence";
                out<<") noexcept {\n"
                     "    [[maybe_unused]] const auto zero=_mm256_setzero_si256();\n"
                     "    [[maybe_unused]] const auto ones=_mm256_set1_epi64x(-1);\n"
                     "    [[maybe_unused]] const std::size_t byte=0;\n";
                const auto& repair=residual?suffix:exact;
                pointer_bases("    ",residual);repair.emit(out,"    ","w",true,true);
                out<<"    return "<<repair.value(residual?residual_plan.suffix.root:plan.root,"w",true,true)<<";\n}\n";
                if(recompute_carries) {
                    out<<attribute<<" "<<macro<<"_NOINLINE inline __m256i evaluate_encoded_repair_avx2_unchecked(\n"
                         "        [[maybe_unused]] const std::array<std::span<const std::uint8_t>,encoded_plane_count>& planes,\n"
                         "        [[maybe_unused]] std::uint32_t offset) noexcept {\n"
                         "    [[maybe_unused]] const auto zero=_mm256_setzero_si256();\n"
                         "    [[maybe_unused]] const auto ones=_mm256_set1_epi64x(-1);\n"
                         "    [[maybe_unused]] const std::size_t byte=0;\n";
                    pointer_bases("    ");carry.emit(out,"    ","c",true,true);
                    out<<"    const EncodedPrefix256 evidence{zero,zero,{";
                    for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                        if(i)out<<",";
                        out<<carry.value(residual_plan.carry_roots[i],"c",true,true);
                    }
                    out<<"}};\n    return evaluate_encoded_residual_avx2_unchecked(planes,offset,evidence);\n}\n";
                    if(narrow_repair) {
                        out<<attribute<<R"CPP( inline unsigned encoded_dirty_word_mask(__m256i unresolved) noexcept {
    // One bit per 64-row word, never one bit per byte or int32 lane.
    const auto clean=_mm256_cmpeq_epi64(unresolved,_mm256_setzero_si256());
    return static_cast<unsigned>(_mm256_movemask_pd(_mm256_castsi256_pd(clean)))^15u;
}
)CPP";
                        out<<attribute<<" "<<macro<<R"CPP(_NOINLINE inline __m256i evaluate_encoded_word_repair_avx2_unchecked(
        const EncodedPlaneViews& planes, std::uint32_t offset,
        __m256i must, __m256i unresolved, unsigned dirty_words) noexcept {
    // The caller supplies complete 256-row views and the exact dirty-word mask.
    // Scalar cold repair reconstructs carries from the same immutable planes.
    // Care-reduced results are merged only under each word's unresolved bits.
    std::array<std::uint64_t,4> result{}, care{};
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(result.data()),must);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(care.data()),unresolved);
    for(unsigned word=0;word<4;++word)if(dirty_words&(1u<<word)) {
        const auto exact=evaluate_encoded_repair_unchecked(planes,offset+word*64,64);
        result[word]|=care[word]&exact;
    }
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(result.data()));
}
)CPP";
                    }
                }
            }
            out<<attribute<<R"CPP( inline std::uint64_t run_encoded_avx2_unchecked(
        const std::array<std::span<const std::uint8_t>,encoded_plane_count>& planes,
        std::uint32_t offset,std::uint32_t count,std::uint64_t* output) noexcept {
    const auto lookup=_mm256_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
                                      0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
    const auto low_nibble=_mm256_set1_epi8(15);
    const auto zero=_mm256_setzero_si256();
    [[maybe_unused]] const auto ones=_mm256_set1_epi64x(-1);
    auto accepted=_mm256_setzero_si256();
)CPP";
            if(!frontier || guarded_carries || recompute_carries)pointer_bases("    ");
            out<<"    for(std::uint32_t row=0;row<count;row+=256) {\n";
            if(frontier) {
                if(guarded_carries || recompute_carries) {
                    out<<"        const auto byte=row/8;\n";
                    const auto& common=recompute_carries?bounds:prefix;
                    common.emit(out,"        ","b",true,true,recompute_carries?nullptr:&bound_nodes,true);
                    out<<"        const auto must="<<common.value(prefix_plan.must_root,"b",true,true)<<";\n"
                       <<"        const auto may="<<common.value(prefix_plan.may_root,"b",true,true)<<";\n"
                       <<"        const auto unresolved=_mm256_xor_si256(must,may);\n"
                       <<"        auto bits=must;\n"
                       <<"        if(!_mm256_testz_si256(unresolved,unresolved)) {\n";
                    if(!recompute_carries) {
                        prefix.emit(out,"            ","b",true,true,&bound_nodes,false);
                        out<<"            const EncodedPrefix256 bounds{must,may,{";
                        for(std::size_t i=0;i<residual_plan.carry_roots.size();++i) {
                            if(i)out<<",";
                            out<<prefix.value(residual_plan.carry_roots[i],"b",true,true);
                        }
                        out<<"}};\n";
                    }
                } else {
                    out<<R"CPP(        const auto bounds=evaluate_encoded_prefix_avx2_unchecked(planes,offset+row);
        const auto unresolved=_mm256_xor_si256(bounds.must,bounds.may);
        auto bits=bounds.must;
        if(!_mm256_testz_si256(unresolved,unresolved)) {
)CPP";
                }
                if(narrow_repair) {
                    out<<"            const auto dirty_words=encoded_dirty_word_mask(unresolved);\n";
                    if(options.encoded_repair_policy==EncodedRepairPolicy::Hybrid)
                        out<<"            if((dirty_words&(dirty_words-1u))==0) {\n";
                    out<<"            bits=evaluate_encoded_word_repair_avx2_unchecked(planes,offset+row,bits,unresolved,dirty_words);\n";
                    if(options.encoded_repair_policy==EncodedRepairPolicy::Hybrid)
                        out<<R"CPP(            } else {
                const auto exact=evaluate_encoded_repair_avx2_unchecked(planes,offset+row);
                bits=_mm256_or_si256(bits,_mm256_and_si256(unresolved,exact));
            }
)CPP";
                } else {
                    out<<"            const auto exact="<<(recompute_carries?"evaluate_encoded_repair_avx2_unchecked(planes,offset+row)":
                        residual?"evaluate_encoded_residual_avx2_unchecked(planes,offset+row,bounds)":
                        "evaluate_encoded_exact_avx2_unchecked(planes,offset+row)")<<";\n";
                    out<<"\n            bits=_mm256_or_si256(bits,_mm256_and_si256(unresolved,exact));\n";
                }
                out<<"        }\n";
            } else {
                out<<"        const auto byte=row/8;\n";
                exact.emit(out,"        ","w",true,true);
                out<<"        const auto bits="<<exact.value(plan.root,"w",true,true)<<";\n";
            }
            out<<R"CPP(        _mm256_storeu_si256(reinterpret_cast<__m256i*>(output+row/64),bits);
        const auto low=_mm256_and_si256(bits,low_nibble);
        const auto high=_mm256_and_si256(_mm256_srli_epi16(bits,4),low_nibble);
        const auto byte_counts=_mm256_add_epi8(_mm256_shuffle_epi8(lookup,low),
                                              _mm256_shuffle_epi8(lookup,high));
        accepted=_mm256_add_epi64(accepted,_mm256_sad_epu8(byte_counts,zero));
    }
    const auto halves=_mm_add_epi64(_mm256_castsi256_si128(accepted),_mm256_extracti128_si256(accepted,1));
    const auto total=_mm_add_epi64(halves,_mm_srli_si128(halves,8));
    std::uint64_t result;
    _mm_storel_epi64(reinterpret_cast<__m128i*>(&result),total);
    return result;
}
#endif
)CPP";
        }
    }
    out<<"inline constexpr std::size_t encoded_common_nodes = "<<metadata.encoded_common_nodes<<";\n";
    out<<"inline constexpr std::size_t encoded_common_planes = "<<metadata.encoded_common_planes<<";\n";
    out<<"inline constexpr std::size_t encoded_common_live_values = "<<metadata.encoded_common_live_values<<";\n";
    out<<"inline constexpr std::size_t encoded_cold_prefix_nodes = "<<metadata.encoded_cold_prefix_nodes<<";\n";
    out<<"inline constexpr std::size_t encoded_cold_prefix_planes = "<<metadata.encoded_cold_prefix_planes<<";\n";
    out<<"inline constexpr std::size_t encoded_cold_prefix_live_values = "<<metadata.encoded_cold_prefix_live_values<<";\n";
    out<<R"CPP(
// Dataset supplies the same verified immutable contract as PackedDataset:
// info(), block_length(), plane_view(), verify_payload(). Output must not alias
// its immutable payload. Original source certificates remain authoritative even
// when encoded lowering removes every read of a constrained source channel.
// Immutable Dataset storage may be shared, but this prepared owner caches views
// and is not reentrant or safe for concurrent calls without external locking.
// Use one prepared instance per worker when sharing the same immutable dataset.
template<class Dataset> class PreparedEncodedKernel {
public:
    explicit PreparedEncodedKernel(const Dataset& data, bool inputs_already_verified=false,
                                   [[maybe_unused]] bool allow_simd=true)
        : data_(data), rows_(data.info().rows), block_rows_(data.info().block_rows) {
)CPP";
    if(complete)out<<R"CPP(        const auto& info=data_.info();
        if(!info.channels || info.ranges.size()!=info.channels || block_rows_<32 || block_rows_>65536 ||
           (block_rows_&(block_rows_-1)))throw std::invalid_argument("generated encoded dimensions are invalid");
        if(!inputs_already_verified)data_.verify_payload();
        for(const auto& contract:load_contracts) {
            if(contract.channel>=info.channels)throw std::invalid_argument("generated encoded channel is unavailable");
            const auto range=info.ranges[contract.channel];
            if(range.min<contract.minimum || range.max>contract.maximum)
                throw std::invalid_argument("generated encoded input violates original source certificate");
        }
        use_simd_=allow_simd && encoded_avx2_emitted && avx2_available() &&
                  std::endian::native==std::endian::little && block_rows_>=256;
)CPP";
    else out<<R"CPP(        (void)inputs_already_verified;
        throw std::invalid_argument("no complete encoded lowering was emitted");
)CPP";
    out<<R"CPP(    }
    [[nodiscard]] bool uses_simd() const noexcept { return use_simd_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t view_storage_bytes() const noexcept { return sizeof(planes_); }
    std::uint64_t run(std::span<std::uint64_t> mask) { return run_range(0,rows_,mask); }
    std::uint64_t run_range([[maybe_unused]] std::uint64_t first, [[maybe_unused]] std::uint64_t count,
                           [[maybe_unused]] std::span<std::uint64_t> mask) {
)CPP";
    if(complete)out<<R"CPP(        if(first>rows_ || count>rows_-first || mask.size()!=mask_words(count))
            throw std::invalid_argument("generated encoded row range or mask shape is invalid");
        if(!count)return 0;
        if constexpr(encoded_plane_count==0) {
            std::uint64_t accepted=0,local=0;
            for(auto& word:mask) {
                const auto length=static_cast<unsigned>(std::min<std::uint64_t>(64,count-local));
                word=evaluate_encoded_unchecked(planes_,0,length);accepted+=std::popcount(word);local+=length;
            }
            return accepted;
        }
        // With word-aligned global starts and physical blocks, every scalar
        // fragment starts a fresh output word; direct assignment initializes it.
        // Other slices can append to a partial word across a block boundary.
        if(block_rows_<64 || first%64!=0)
            std::fill(mask.begin(),mask.end(),std::uint64_t{0});
        const auto end=first+count;
        std::uint64_t accepted=0,global=first;
        while(global<end) {
            const auto block=global/block_rows_;
            const auto offset=static_cast<std::uint32_t>(global%block_rows_);
            const auto physical_length=bind_encoded_planes(data_,block,planes_);
            const auto length=static_cast<std::uint32_t>(std::min<std::uint64_t>(end-global,physical_length-offset));
            std::uint32_t consumed=0;
)CPP";
    if(complete && vector_encoded) out << "#if " << macro << "_X86\n" << R"CPP(            if(use_simd_ && offset%8==0 && (global-first)%64==0 && length>=256) {
                consumed=length&~std::uint32_t{255};
                accepted+=run_encoded_avx2_unchecked(planes_,offset,consumed,
                    mask.data()+static_cast<std::size_t>((global-first)/64));
            }
#endif
)CPP";
    if(complete)out<<R"CPP(            for(;consumed<length;) {
                const auto take=std::min<unsigned>(64,length-consumed);
                const auto bits=evaluate_encoded_unchecked(planes_,offset+consumed,take);
                const auto local=global-first+consumed;
                const auto shift=static_cast<unsigned>(local%64);
                const auto word=static_cast<std::size_t>(local/64);
                if(!shift)mask[word]=bits;
                else {
                    mask[word]|=bits<<shift;
                    if(take>64-shift)mask[word+1]|=bits>>(64-shift);
                }
                accepted+=std::popcount(bits);consumed+=take;
            }
            global+=length;
        }
        return accepted;
)CPP";
    else out<<R"CPP(        throw std::logic_error("no complete encoded lowering was emitted");
)CPP";
    out<<R"CPP(    }
private:
    const Dataset& data_;
    std::uint64_t rows_{};
    std::uint32_t block_rows_{};
    bool use_simd_{};
    EncodedPlaneViews planes_{};
};
)CPP";
}

} // namespace

GeneratedKernel emit_cpp(const CompiledProgram& plan, CodegenOptions options) {
    if (!plan.node_count()) throw std::invalid_argument("code generation requires a compiled program");
    if (!valid_namespace(options.namespace_name)) throw std::invalid_argument("invalid or reserved generated namespace");
    if(options.encoded_prefix_bits>32)throw std::invalid_argument("encoded prefix depth must be in 0..32");
    if(options.encoded_strategy!=EncodedStrategy::Eager && options.encoded_strategy!=EncodedStrategy::Frontier &&
       options.encoded_strategy!=EncodedStrategy::Residual && options.encoded_strategy!=EncodedStrategy::GuardedResidual)
        throw std::invalid_argument("invalid encoded strategy");
    if(options.encoded_schedule!=EncodedSchedule::Legacy && options.encoded_schedule!=EncodedSchedule::Pressure &&
       options.encoded_schedule!=EncodedSchedule::BoundsFirst)
        throw std::invalid_argument("invalid encoded schedule");
    if(options.raw_layout!=RawLayout::Interleaved && options.raw_layout!=RawLayout::Planar)
        throw std::invalid_argument("invalid generated raw layout");
    if(options.encoded_plane_binding!=EncodedPlaneBinding::PerPlane && options.encoded_plane_binding!=EncodedPlaneBinding::Grouped)
        throw std::invalid_argument("invalid encoded plane binding");
    if(options.encoded_repair_policy!=EncodedRepairPolicy::Batch256 && options.encoded_repair_policy!=EncodedRepairPolicy::Word64 &&
       options.encoded_repair_policy!=EncodedRepairPolicy::Hybrid)
        throw std::invalid_argument("invalid encoded repair policy");
    if(options.encoded_repair_policy!=EncodedRepairPolicy::Batch256 &&
       (!residual_strategy(options.encoded_strategy) || options.encoded_carry_placement!=EncodedCarryPlacement::Recompute))
        throw std::invalid_argument("word64 and hybrid repair require a residual strategy and recompute carry placement");
    if(options.encoded_carry_placement!=EncodedCarryPlacement::Eager && options.encoded_carry_placement!=EncodedCarryPlacement::Guarded &&
       options.encoded_carry_placement!=EncodedCarryPlacement::Recompute)
        throw std::invalid_argument("invalid encoded carry placement");
    if(options.encoded_carry_placement==EncodedCarryPlacement::Guarded && !residual_strategy(options.encoded_strategy))
        throw std::invalid_argument("guarded carry placement requires a residual strategy");
    if(options.encoded_carry_placement==EncodedCarryPlacement::Recompute && !residual_strategy(options.encoded_strategy))
        throw std::invalid_argument("recompute carry placement requires a residual strategy");
    const auto predicate = options.lower_predicates ? compile_predicate(plan) : PredicatePlan{};
    const auto wordcode = options.enable_encoded && predicate.complete ? compile_wordcode(predicate) : WordcodePlan{};
    const auto frontier=options.enable_encoded && predicate.complete && options.encoded_strategy==EncodedStrategy::Frontier?
        compile_frontier(predicate,options.encoded_prefix_bits):FrontierPlan{};
    const auto residual=options.enable_encoded && predicate.complete && residual_strategy(options.encoded_strategy)?
        (options.encoded_strategy==EncodedStrategy::GuardedResidual?
         compile_guarded_residual(predicate,options.encoded_prefix_bits,default_wordcode_node_budget,max_residual_carries,options.guarded_residual_options):
         compile_residual(predicate,options.encoded_prefix_bits)):ResidualPlan{};
    const bool encoded=wordcode.complete && (options.encoded_strategy==EncodedStrategy::Eager || frontier.complete || residual.complete);
    if(options.encoded_repair_policy!=EncodedRepairPolicy::Batch256 && !encoded)
        throw std::invalid_argument("word64 and hybrid repair require complete encoded residual lowering");
    const bool staged=encoded && options.encoded_strategy!=EncodedStrategy::Eager;
    const auto& prefix=residual_strategy(options.encoded_strategy)?residual.prefix:frontier.prefix;
    GeneratedKernel result;
    result.predicate_lowered = predicate.complete;
    result.avx2_emitted = options.enable_avx2;
    result.encoded_emitted = encoded;
    result.encoded_frontier_emitted=encoded && options.encoded_strategy==EncodedStrategy::Frontier;
    result.encoded_residual_emitted=encoded && residual_strategy(options.encoded_strategy);
    result.encoded_guarded_residual_emitted=encoded && options.encoded_strategy==EncodedStrategy::GuardedResidual;
    result.encoded_guarded_carry_emitted=result.encoded_residual_emitted && options.encoded_carry_placement==EncodedCarryPlacement::Guarded;
    result.encoded_recompute_carry_emitted=result.encoded_residual_emitted && options.encoded_carry_placement==EncodedCarryPlacement::Recompute;
    result.encoded_residual_exact_all_lanes=result.encoded_residual_emitted && residual.scope==ResidualScope::AllLanes;
    result.encoded_avx2_emitted = encoded && options.enable_avx2 &&
        (result.encoded_residual_emitted?(prefix.plane_count!=0 || residual.suffix.plane_count!=0):
        (wordcode.plane_count != 0 || (result.encoded_frontier_emitted && frontier.prefix.plane_count != 0)));
    result.source_nodes = static_cast<std::uint32_t>(plan.node_count());
    result.predicate_nodes = predicate.complete ? predicate.node_count : 0;
    result.wordcode_nodes = encoded ? static_cast<std::uint32_t>(wordcode.nodes.size()) : 0;
    result.encoded_prefix_bits=staged?options.encoded_prefix_bits:0;
    result.encoded_prefix_nodes=staged?static_cast<std::uint32_t>(prefix.nodes.size()):0;
    result.encoded_prefix_planes=staged?prefix.plane_count:0;
    result.encoded_residual_nodes=result.encoded_residual_emitted?static_cast<std::uint32_t>(residual.suffix.nodes.size()):0;
    result.encoded_residual_planes=result.encoded_residual_emitted?residual.suffix.plane_count:0;
    result.encoded_residual_carries=result.encoded_residual_emitted?static_cast<std::uint32_t>(residual.carry_roots.size()):0;
    result.encoded_residual_initial_carries=result.encoded_residual_emitted?
        (result.encoded_guarded_residual_emitted?residual.proof.initial_carries:result.encoded_residual_carries):0;
    if(result.encoded_guarded_residual_emitted) {
        result.encoded_proof_attempts=residual.proof.attempts;
        result.encoded_proof_accepted_rewrites=residual.proof.accepted_rewrites;
        result.encoded_proof_bdd_nodes=residual.proof.bdd_nodes;
        result.encoded_proof_graph_budget_rejections=residual.proof.graph_budget_rejections;
        result.encoded_proof_steps=residual.proof.proof_steps;
        result.encoded_proof_exhausted=residual.proof.exhausted;
        result.encoded_proof_reason=residual.proof.reason;
    }
    if(!encoded)result.encoded_reason=!options.enable_encoded?"encoded generation disabled":
        !predicate.complete?predicate.reason:!wordcode.complete?wordcode.reason:
        residual_strategy(options.encoded_strategy)?residual.reason:frontier.reason;
    result.description = std::string("AOT fused ") + (predicate.complete ? "Boolean predicate" : "original int64 arithmetic") +
                         "; scalar" + (options.enable_avx2 ? (predicate.complete ? " and 8-lane AVX2" : " and 4-lane AVX2") : " only") +
                         (encoded ? (result.encoded_avx2_emitted ? "; encoded scalar and 256-row AVX2" : "; encoded scalar") : "") +
                         (result.encoded_frontier_emitted?"; certified prefix frontier "+std::to_string(options.encoded_prefix_bits):"") +
                         (result.encoded_residual_emitted?(result.encoded_guarded_residual_emitted?"; suffix residual exact on unresolved lanes after prefix ":"; all-lane exact suffix residual after prefix ")+std::to_string(options.encoded_prefix_bits):"") +
                         (result.encoded_guarded_carry_emitted?"; carry materialization guarded in generated source":"") +
                         (result.encoded_recompute_carry_emitted?"; carry reconstruction in a non-inlined repair helper":"") +
                         (encoded && options.encoded_schedule==EncodedSchedule::Pressure?"; pressure schedule with plane rematerialization":"") +
                         (encoded && options.encoded_schedule==EncodedSchedule::BoundsFirst?"; bounds-first schedule with plane rematerialization":"") +
                         (encoded && options.encoded_plane_binding==EncodedPlaneBinding::Grouped?"; grouped contiguous plane binding":"") +
                         (encoded && options.encoded_repair_policy==EncodedRepairPolicy::Word64?"; dirty 64-row word repair":"") +
                         (encoded && options.encoded_repair_policy==EncodedRepairPolicy::Hybrid?"; one-dirty-word hybrid repair":"") +
                         (options.raw_layout==RawLayout::Planar?"; planar raw layout":"; interleaved raw layout") +
                         "; global row ranges; validated source contracts";
    const auto macro = "SUFFICE_CG_" + options.namespace_name;
    const auto attribute = macro + "_AVX2";
    std::ostringstream out;
    out << R"CPP(// Generated by Suffice. Compile this header as C++20.
// Input is immutable host-native int32 values in the generated raw_layout. PreparedKernel
// validates dimensions and the exact source range contract once. The *_unchecked
// functions require that same contract, valid ranges/mask sizes, and no input /
// output aliasing. Arithmetic is exact within the certified int64 ranges.
// The raw AVX2 unchecked entry additionally requires CPU/OS AVX2 support and
// channels <= INT32_MAX/7 so signed gather indices cannot overflow.
// This is ahead-of-time generated code, not an executable-memory JIT.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
)CPP";
    out << "#define " << macro << "_X86 1\n#else\n#define " << macro << "_X86 0\n#endif\n";
    out << "#if " << macro << "_X86 && (defined(__GNUC__) || defined(__clang__))\n#define " << attribute
        << " __attribute__((target(\"avx2\")))\n#else\n#define " << attribute << "\n#endif\n";
    out<<"#if defined(_MSC_VER)\n#define "<<macro<<"_NOINLINE __declspec(noinline)\n"
         "#elif defined(__GNUC__) || defined(__clang__)\n#define "<<macro<<"_NOINLINE __attribute__((noinline))\n"
         "#else\n#define "<<macro<<"_NOINLINE\n#endif\n";
    out << "namespace " << options.namespace_name << " {\n";
    out<<"inline constexpr bool raw_planar = "<<(options.raw_layout==RawLayout::Planar?"true":"false")<<";\n";
    out<<"inline constexpr const char* raw_layout = \""<<(options.raw_layout==RawLayout::Planar?"planar":"interleaved")<<"\";\n";
    out << "inline constexpr bool predicate_lowered = " << (predicate.complete ? "true" : "false") << ";\n";
    out << "inline constexpr bool avx2_emitted = " << (options.enable_avx2 ? "true" : "false") << ";\n";
    out << "inline constexpr std::size_t source_nodes = " << plan.node_count() << ";\n";
    out << "inline constexpr std::size_t predicate_nodes = " << result.predicate_nodes << ";\n";
    out << "inline constexpr std::int64_t threshold = " << literal(plan.threshold()) << ";\n";
    out << "inline constexpr unsigned simd_lanes = " << (predicate.complete ? 8 : 4) << ";\n";
    const auto bounds = plan.output_bounds();
    out << "inline constexpr int constant_decision = " << (bounds.lo > plan.threshold() ? 1 : bounds.hi <= plan.threshold() ? 0 : -1) << ";\n";
    out << "struct LoadContract { std::uint32_t channel; std::int64_t minimum, maximum; };\n";
    std::vector<Load> contracts;
    for (const auto& load : plan.loads()) {
        bool duplicate = false;
        for (const auto& previous : contracts) if (previous.channel == load.channel) duplicate = true;
        if (!duplicate) contracts.push_back(load);
    }
    out << "inline constexpr std::array<LoadContract, " << contracts.size() << "> load_contracts{{\n";
    for (const auto& load : contracts) out << "    {" << load.channel << "u, " << literal(load.range.lo) << ", " << literal(load.range.hi) << "},\n";
    out << "}};\n";
    out << R"CPP(
inline std::size_t mask_words(std::uint64_t rows) {
    const auto words = rows / 64 + (rows % 64 != 0);
    if (words > std::numeric_limits<std::size_t>::max()) throw std::length_error("generated mask is too large");
    return static_cast<std::size_t>(words);
}
inline std::uint64_t clamp_row(std::uint64_t row, int offset, std::uint64_t rows) noexcept {
    if (offset < 0) { const auto n = static_cast<std::uint64_t>(-offset); return row < n ? 0 : row - n; }
    const auto n = static_cast<std::uint64_t>(offset);
    return n >= rows || row >= rows - n ? rows - 1 : row + n;
}
inline std::uint64_t raw_index(std::uint64_t row,std::uint32_t channel,
                              std::uint64_t rows,std::uint32_t channels) noexcept {
    if constexpr(raw_planar)return std::uint64_t(channel)*rows+row;
    else return row*channels+channel;
}
inline std::uint64_t low_mask(unsigned bits) noexcept {
    return bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}
inline void validate_input(std::span<const std::int32_t> values, std::uint64_t rows, std::uint32_t channels) {
    if (!channels || rows > std::numeric_limits<std::size_t>::max() / channels ||
        values.size() != static_cast<std::size_t>(rows * channels))
        throw std::invalid_argument("generated input dimensions do not match values");
    for (const auto& load : load_contracts) {
        if (load.channel >= channels) throw std::invalid_argument("generated input channel is unavailable");
        for (std::uint64_t row = 0; row < rows; ++row) {
            const auto value = values[static_cast<std::size_t>(raw_index(row,load.channel,rows,channels))];
            if (value < load.minimum || value > load.maximum)
                throw std::invalid_argument("generated input violates compiled range contract");
        }
    }
}
inline bool avx2_available() noexcept {
)CPP";
    out << "#if " << macro << "_X86 && (defined(__GNUC__) || defined(__clang__))\n"
        << "    return __builtin_cpu_supports(\"avx2\");\n"
        << "#elif " << macro << "_X86 && defined(_MSC_VER)\n"
        << "    int registers[4]{}; __cpuid(registers, 0); if (registers[0] < 7) return false;\n"
        << "    __cpuidex(registers, 1, 0); if ((registers[2] & 0x18000000) != 0x18000000) return false;\n"
        << "    if ((_xgetbv(0) & 6) != 6) return false; __cpuidex(registers, 7, 0); return (registers[1] & 32) != 0;\n"
        << "#else\n    return false;\n#endif\n}\n";
    emit_scalar(out, plan, predicate, predicate.complete);
    out << R"CPP(
inline std::uint64_t run_scalar_unchecked(const std::int32_t* values, std::uint64_t rows,
        std::uint32_t channels, std::uint64_t first, std::uint64_t count,
        std::span<std::uint64_t> mask) noexcept {
    std::uint64_t accepted = 0, local = 0;
    for (auto& word : mask) {
        const auto length = static_cast<unsigned>(std::min<std::uint64_t>(64, count - local));
        if constexpr (constant_decision >= 0) word = constant_decision ? low_mask(length) : 0;
        else {
            word = 0;
            for (unsigned bit = 0; bit < length; ++bit)
                word |= std::uint64_t(evaluate_scalar(values, first + local + bit, rows, channels)) << bit;
        }
        accepted += std::popcount(word); local += length;
    }
    return accepted;
}
)CPP";
    if (options.enable_avx2) {
        out << "#if " << macro << "_X86\n";
        out << attribute << R"CPP( inline __m256i load8(const std::int32_t* values, std::uint64_t row,
        std::uint64_t rows,std::uint32_t channels, std::uint32_t channel) noexcept {
    if constexpr(raw_planar)return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values+std::uint64_t(channel)*rows+row));
    const auto* first = values + row * channels + channel;
    if (channels == 1) return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(first));
    const auto c = static_cast<std::int32_t>(channels);
    return _mm256_i32gather_epi32(first, _mm256_setr_epi32(0,c,2*c,3*c,4*c,5*c,6*c,7*c), 4);
}
)CPP";
        out << R"CPP(struct Inputs2 { __m256i c0, c1; };
struct Inputs8 { __m256i c0, c1, c2, c3, c4, c5, c6, c7; };
)CPP";
        out << attribute << R"CPP( inline Inputs2 load_two_channels(const std::int32_t* values, std::uint64_t row) noexcept {
    const auto* p=values+row*2;
    const auto order=_mm256_setr_epi32(0,2,4,6,1,3,5,7);
    const auto a=_mm256_permutevar8x32_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)),order);
    const auto b=_mm256_permutevar8x32_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+8)),order);
    return {_mm256_permute2x128_si256(a,b,0x20),_mm256_permute2x128_si256(a,b,0x31)};
}
)CPP";
        out << attribute << R"CPP( inline Inputs8 load_eight_channels(const std::int32_t* values, std::uint64_t row) noexcept {
    const auto* p=values+row*8;
    const auto a0=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const auto a1=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+8));
    const auto a2=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+16));
    const auto a3=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+24));
    const auto a4=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+32));
    const auto a5=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+40));
    const auto a6=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+48));
    const auto a7=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+56));
    const auto b0=_mm256_unpacklo_epi32(a0,a1),b1=_mm256_unpackhi_epi32(a0,a1);
    const auto b2=_mm256_unpacklo_epi32(a2,a3),b3=_mm256_unpackhi_epi32(a2,a3);
    const auto b4=_mm256_unpacklo_epi32(a4,a5),b5=_mm256_unpackhi_epi32(a4,a5);
    const auto b6=_mm256_unpacklo_epi32(a6,a7),b7=_mm256_unpackhi_epi32(a6,a7);
    const auto d0=_mm256_unpacklo_epi64(b0,b2),d1=_mm256_unpackhi_epi64(b0,b2);
    const auto d2=_mm256_unpacklo_epi64(b1,b3),d3=_mm256_unpackhi_epi64(b1,b3);
    const auto d4=_mm256_unpacklo_epi64(b4,b6),d5=_mm256_unpackhi_epi64(b4,b6);
    const auto d6=_mm256_unpacklo_epi64(b5,b7),d7=_mm256_unpackhi_epi64(b5,b7);
    return {_mm256_permute2x128_si256(d0,d4,0x20),_mm256_permute2x128_si256(d1,d5,0x20),
            _mm256_permute2x128_si256(d2,d6,0x20),_mm256_permute2x128_si256(d3,d7,0x20),
            _mm256_permute2x128_si256(d0,d4,0x31),_mm256_permute2x128_si256(d1,d5,0x31),
            _mm256_permute2x128_si256(d2,d6,0x31),_mm256_permute2x128_si256(d3,d7,0x31)};
}
)CPP";
        out << attribute << R"CPP( inline __m256i load4(const std::int32_t* values, std::uint64_t row,
        std::uint64_t rows, std::uint32_t channels, std::uint32_t channel, int offset) noexcept {
    bool interior;
    std::uint64_t shifted;
    if (offset < 0) {
        const auto n = static_cast<std::uint64_t>(-offset); interior = row >= n; shifted = interior ? row - n : 0;
    } else {
        const auto n = static_cast<std::uint64_t>(offset);
        interior = n < rows && row + 3 < rows - n; shifted = interior ? row + n : 0;
    }
    if (interior) {
        if constexpr(raw_planar)return _mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(values+std::uint64_t(channel)*rows+shifted)));
        const auto* first = values + shifted * channels + channel;
        if (channels == 1) return _mm256_cvtepi32_epi64(_mm_loadu_si128(reinterpret_cast<const __m128i*>(first)));
        const auto c = static_cast<std::int32_t>(channels);
        return _mm256_cvtepi32_epi64(_mm_i32gather_epi32(first, _mm_setr_epi32(0,c,2*c,3*c), 4));
    }
    return _mm256_setr_epi64x(
        values[raw_index(clamp_row(row, offset, rows),channel,rows,channels)],
        values[raw_index(clamp_row(row+1, offset, rows),channel,rows,channels)],
        values[raw_index(clamp_row(row+2, offset, rows),channel,rows,channels)],
        values[raw_index(clamp_row(row+3, offset, rows),channel,rows,channels)]);
}
)CPP";
        out << attribute << R"CPP( inline __m256i multiply64(__m256i a, __m256i b) noexcept {
    const auto low = _mm256_mul_epu32(a,b);
    const auto cross = _mm256_add_epi64(_mm256_mul_epu32(_mm256_srli_epi64(a,32), b),
                                     _mm256_mul_epu32(a, _mm256_srli_epi64(b,32)));
    return _mm256_add_epi64(low, _mm256_slli_epi64(cross,32));
}
)CPP";
        out << attribute << R"CPP( inline __m256i absolute64(__m256i a) noexcept {
    const auto negative = _mm256_cmpgt_epi64(_mm256_setzero_si256(),a);
    return _mm256_sub_epi64(_mm256_xor_si256(a,negative),negative);
}
)CPP";
        out << attribute << R"CPP( inline __m256i minimum64(__m256i a, __m256i b) noexcept {
    return _mm256_blendv_epi8(a,b,_mm256_cmpgt_epi64(a,b));
}
)CPP";
        out << attribute << R"CPP( inline __m256i maximum64(__m256i a, __m256i b) noexcept {
    return _mm256_blendv_epi8(b,a,_mm256_cmpgt_epi64(a,b));
}
)CPP";
        emit_vector(out, plan, predicate, predicate.complete, attribute);
        out << attribute << R"CPP( inline std::uint64_t run_avx2_unchecked(
        const std::int32_t* values, std::uint64_t rows, std::uint32_t channels,
        std::uint64_t first, std::uint64_t count, std::span<std::uint64_t> mask) noexcept {
    if constexpr (constant_decision >= 0) return run_scalar_unchecked(values, rows, channels, first, count, mask);
    std::uint64_t accepted = 0, local = 0;
    for (auto& word : mask) {
        const auto length = static_cast<unsigned>(std::min<std::uint64_t>(64, count-local));
        word = 0;
        unsigned bit = 0;
        for (; bit + simd_lanes <= length; bit += simd_lanes)
            word |= std::uint64_t(evaluate_simd(values, first+local+bit, rows, channels)) << bit;
        for (; bit < length; ++bit)
            word |= std::uint64_t(evaluate_scalar(values, first+local+bit, rows, channels)) << bit;
        accepted += std::popcount(word); local += length;
    }
    return accepted;
}
#endif
)CPP";
    }
    out << R"CPP(
class PreparedKernel {
public:
    PreparedKernel(std::span<const std::int32_t> values, std::uint64_t rows,
                   std::uint32_t channels, bool allow_simd = true)
        : values_(values), rows_(rows), channels_(channels) {
        validate_input(values, rows, channels);
        // Signed 32-bit gather indices must not overflow. Very wide input
        // records remain valid and fall back to the fused scalar kernel.
        use_simd_ = allow_simd && avx2_emitted && avx2_available() &&
                    channels <= std::uint32_t(std::numeric_limits<std::int32_t>::max() / 7);
    }
    [[nodiscard]] bool uses_simd() const noexcept { return use_simd_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return rows_; }
    std::uint64_t run(std::span<std::uint64_t> mask) const { return run_range(0, rows_, mask); }
    std::uint64_t run_range(std::uint64_t first, std::uint64_t count,
                            std::span<std::uint64_t> mask) const {
        if (first > rows_ || count > rows_ - first || mask.size() != mask_words(count))
            throw std::invalid_argument("generated row range or output shape is invalid");
        if (!count) return 0;
        const auto begin = reinterpret_cast<std::uintptr_t>(values_.data());
        const auto end = begin + values_.size_bytes();
        const auto out_begin = reinterpret_cast<std::uintptr_t>(mask.data());
        const auto out_end = out_begin + mask.size_bytes();
        if (begin < out_end && out_begin < end)
            throw std::invalid_argument("generated input and output must not overlap");
)CPP";
    if (options.enable_avx2) out << "#if " << macro << "_X86\n        if (use_simd_) return run_avx2_unchecked(values_.data(), rows_, channels_, first, count, mask);\n#endif\n";
    out << R"CPP(        return run_scalar_unchecked(values_.data(), rows_, channels_, first, count, mask);
    }
private:
    std::span<const std::int32_t> values_;
    std::uint64_t rows_{};
    std::uint32_t channels_{};
    bool use_simd_{};
};
)CPP";
    emit_encoded(out,wordcode,frontier,residual,options,macro,attribute,result);
    out << "} // namespace " << options.namespace_name << "\n#undef " << attribute << "\n#undef " << macro << "_X86\n#undef "<<macro<<"_NOINLINE\n";
    result.header = std::move(out).str();
    return result;
}

} // namespace suffice
