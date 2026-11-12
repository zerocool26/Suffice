#include "suffice/residual.hpp"
#include "suffice/boolean_proof.hpp"

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
bool operation(ResidualOp op) noexcept {
    return op==ResidualOp::Not || op==ResidualOp::And || op==ResidualOp::Or;
}
bool binary(ResidualOp op) noexcept { return op==ResidualOp::And || op==ResidualOp::Or; }
ResidualOp convert(WordcodeOp op) {
    switch(op) {
    case WordcodeOp::False:return ResidualOp::False;
    case WordcodeOp::True:return ResidualOp::True;
    case WordcodeOp::Plane:return ResidualOp::Plane;
    case WordcodeOp::And:return ResidualOp::And;
    case WordcodeOp::Or:return ResidualOp::Or;
    case WordcodeOp::Not:return ResidualOp::Not;
    }
    throw Failure{"invalid atomic wordcode operation"};
}
WordcodeOp convert(ResidualOp op) {
    switch(op) {
    case ResidualOp::False:return WordcodeOp::False;
    case ResidualOp::True:return WordcodeOp::True;
    case ResidualOp::Plane:return WordcodeOp::Plane;
    case ResidualOp::And:return WordcodeOp::And;
    case ResidualOp::Or:return WordcodeOp::Or;
    case ResidualOp::Not:return WordcodeOp::Not;
    case ResidualOp::Carry:break;
    }
    throw Failure{"a prefix forest cannot consume a residual carry"};
}
struct Hash {
    std::size_t operator()(const ResidualNode& n) const noexcept {
        auto h=std::uint64_t(n.a)*0x9e3779b185ebca87ULL;
        h^=std::uint64_t(n.b)*0xc2b2ae3d27d4eb4fULL;
        h^=std::uint64_t(n.load_index)*0x165667b19e3779f9ULL;
        h^=std::uint64_t(n.carry_index)*0x85ebca77c2b2ae63ULL;
        h^=(std::uint64_t(n.bit)<<8)|static_cast<unsigned>(n.op);
        return static_cast<std::size_t>(h^(h>>33));
    }
};

// Bounded structural hashing, constant folding, complement and absorption
// identities. This is a Boolean DAG, not a canonical BDD or exact synthesis.
class Graph {
public:
    explicit Graph(std::uint32_t budget):budget_(budget) {}
    ResidualStage result;
    std::uint32_t constant(bool value) {return intern({value?ResidualOp::True:ResidualOp::False});}
    bool is(std::uint32_t id,bool value) const {return result.nodes[id].op==(value?ResidualOp::True:ResidualOp::False);}
    std::uint32_t plane(std::uint16_t load,std::uint8_t bit) {return intern({ResidualOp::Plane,0,0,load,bit});}
    std::uint32_t carry(std::uint16_t id) {return intern({ResidualOp::Carry,0,0,0,0,id});}
    std::uint32_t negate(std::uint32_t id) {
        if(is(id,false))return constant(true);
        if(is(id,true))return constant(false);
        if(result.nodes[id].op==ResidualOp::Not)return result.nodes[id].a;
        return intern({ResidualOp::Not,id});
    }
    std::uint32_t combine(ResidualOp op,std::uint32_t a,std::uint32_t b) {
        if(a==b)return a;
        const bool conjunction=op==ResidualOp::And;
        if(is(a,!conjunction))return a;
        if(is(b,!conjunction))return b;
        if(is(a,conjunction))return b;
        if(is(b,conjunction))return a;
        if((result.nodes[a].op==ResidualOp::Not && result.nodes[a].a==b) ||
           (result.nodes[b].op==ResidualOp::Not && result.nodes[b].a==a))return constant(!conjunction);
        const auto other=conjunction?ResidualOp::Or:ResidualOp::And;
        const auto contains=[&](std::uint32_t id,std::uint32_t child) {
            const auto& n=result.nodes[id];return n.op==other && (n.a==child || n.b==child);
        };
        if(contains(a,b))return b;
        if(contains(b,a))return a;
        if(a>b)std::swap(a,b);
        return intern({op,a,b});
    }
    // Preserve ALL roots. Reusing a slot at its final operand use is safe
    // because an executor reads operands before overwriting its destination.
    void finish(std::vector<std::uint32_t>& roots) {
        std::vector<bool> needed(result.nodes.size());
        for(const auto root:roots)needed[root]=true;
        for(std::size_t i=result.nodes.size();i--;)if(needed[i]) {
            const auto& n=result.nodes[i];
            if(operation(n.op))needed[n.a]=true;
            if(binary(n.op))needed[n.b]=true;
        }
        std::vector<std::uint32_t> map(result.nodes.size());
        std::vector<ResidualNode> kept;
        for(std::size_t i=0;i<result.nodes.size();++i)if(needed[i]) {
            auto n=result.nodes[i];
            if(operation(n.op))n.a=map[n.a];
            if(binary(n.op))n.b=map[n.b];
            map[i]=static_cast<std::uint32_t>(kept.size());kept.push_back(n);
        }
        for(auto& root:roots)root=map[root];
        result.nodes=std::move(kept);
        std::vector<std::uint32_t> last(result.nodes.size()),owner(result.nodes.size());
        for(std::uint32_t i=0;i<result.nodes.size();++i) {
            last[i]=i;
            const auto& n=result.nodes[i];
            if(operation(n.op))last[n.a]=i;
            if(binary(n.op))last[n.b]=i;
        }
        for(const auto root:roots)last[root]=static_cast<std::uint32_t>(result.nodes.size());
        for(std::uint32_t i=0;i<result.nodes.size();++i) {
            auto& n=result.nodes[i];
            if(n.op==ResidualOp::Plane) {
                ++result.plane_count;
                result.maximum_depth=std::max(result.maximum_depth,unsigned(n.bit)+1);
            } else if(operation(n.op)) {
                ++result.operation_count;
                std::uint32_t slot=0;
                for(;slot<result.live_slot_count;++slot)if(last[owner[slot]]<=i)break;
                if(slot==result.live_slot_count)++result.live_slot_count;
                owner[slot]=i;n.slot=slot;
            }
        }
    }
private:
    std::uint32_t budget_;
    std::unordered_map<ResidualNode,std::uint32_t,Hash> interned_;
    std::uint32_t intern(ResidualNode n) {
        if(const auto found=interned_.find(n);found!=interned_.end())return found->second;
        if(result.nodes.size()>=budget_)throw Failure{"residual graph node budget exhausted; no partial plan is executable"};
        const auto id=static_cast<std::uint32_t>(result.nodes.size());
        result.nodes.push_back(n);interned_.emplace(n,id);return id;
    }
};

class Compiler {
public:
    Compiler(const PredicatePlan& source,unsigned depth,std::uint32_t budget,std::uint32_t carry_budget,
             WordcodePlan exact):source_(source),depth_(depth),budget_(budget),carry_budget_(carry_budget),
             prefix_(budget),suffix_(budget) {
        result_.prefix_bits=depth;result_.exact=std::move(exact);
        result_.prefix.loads=result_.exact.loads;
        for(std::uint16_t i=0;i<source.load_count;++i) {
            const auto channel=source.loads[i].channel;
            const auto& loads=result_.exact.loads;
            const auto it=std::find_if(loads.begin(),loads.end(),[&](const Load& l){return l.channel==channel;});
            if(it==loads.end())throw Failure{"residual lost a source range certificate"};
            load_map_[i]=static_cast<std::uint16_t>(it-loads.begin());
        }
    }
    ResidualPlan compile() {
        const auto exact_op=result_.exact.nodes[result_.exact.root].op;
        std::uint32_t lower_root{},upper_root{},exact_root{};
        if(exact_op==WordcodeOp::False || exact_op==WordcodeOp::True) {
            lower_root=upper_root=prefix_.constant(exact_op==WordcodeOp::True);
            exact_root=suffix_.constant(exact_op==WordcodeOp::True);
        } else {
            std::array<std::uint32_t,max_predicate_nodes> lower{},upper{},truth{};
            for(std::size_t i=0;i<source_.node_count;++i) {
                const auto& n=source_.nodes[i];
                switch(n.op) {
                case PredicateOp::False:case PredicateOp::True:
                    lower[i]=upper[i]=prefix_.constant(n.op==PredicateOp::True);
                    truth[i]=suffix_.constant(n.op==PredicateOp::True);break;
                case PredicateOp::Greater: {
                    const auto load=load_map_[n.load_index];
                    const auto [g,may]=atom_bounds(load,n.threshold);
                    const auto e=prefix_.combine(ResidualOp::And,may,prefix_.negate(g));
                    lower[i]=g;upper[i]=may;
                    // E=0 eliminates the suffix entirely (including max-suffix
                    // strict thresholds). Constants never consume carry slots.
                    if(prefix_.is(e,false))truth[i]=carry(g);
                    else {
                        const auto s=suffix_compare(load,n.threshold);
                        const auto uncertain=suffix_.combine(ResidualOp::And,carry(e),s);
                        truth[i]=suffix_.combine(ResidualOp::Or,carry(g),uncertain);
                    }
                    break;
                }
                case PredicateOp::And:case PredicateOp::Or: {
                    const auto op=n.op==PredicateOp::And?ResidualOp::And:ResidualOp::Or;
                    lower[i]=prefix_.combine(op,lower[n.a],lower[n.b]);
                    upper[i]=prefix_.combine(op,upper[n.a],upper[n.b]);
                    truth[i]=suffix_.combine(op,truth[n.a],truth[n.b]);break;
                }
                case PredicateOp::Not:
                    lower[i]=prefix_.negate(upper[n.a]);upper[i]=prefix_.negate(lower[n.a]);
                    truth[i]=suffix_.negate(truth[n.a]);break;
                }
            }
            lower_root=lower[source_.root];upper_root=upper[source_.root];exact_root=truth[source_.root];
        }
        std::vector<std::uint32_t> suffix_roots{exact_root};suffix_.finish(suffix_roots);
        suffix_.result.root=suffix_roots[0];
        std::vector<bool> used(result_.carry_roots.size());
        for(const auto& n:suffix_.result.nodes)if(n.op==ResidualOp::Carry)used[n.carry_index]=true;
        std::vector<std::uint16_t> carry_map(used.size());
        std::vector<std::uint32_t> kept;
        for(std::size_t i=0;i<used.size();++i)if(used[i]) {
            carry_map[i]=static_cast<std::uint16_t>(kept.size());kept.push_back(result_.carry_roots[i]);
        }
        result_.carry_roots=std::move(kept);
        for(auto& n:suffix_.result.nodes)if(n.op==ResidualOp::Carry)n.carry_index=carry_map[n.carry_index];
        std::vector<std::uint32_t> roots{lower_root,upper_root};
        roots.insert(roots.end(),result_.carry_roots.begin(),result_.carry_roots.end());
        prefix_.finish(roots);
        result_.prefix.must_root=roots[0];result_.prefix.may_root=roots[1];
        result_.carry_roots.assign(roots.begin()+2,roots.end());
        const auto& p=prefix_.result;
        result_.prefix.plane_count=p.plane_count;result_.prefix.operation_count=p.operation_count;
        result_.prefix.live_slot_count=p.live_slot_count;result_.prefix.maximum_depth=p.maximum_depth;
        for(const auto& n:p.nodes)
            result_.prefix.nodes.push_back({convert(n.op),n.a,n.b,n.load_index,n.bit,n.slot});
        result_.suffix=std::move(suffix_.result);result_.complete=true;
        return std::move(result_);
    }
private:
    const PredicatePlan& source_;
    unsigned depth_;
    std::uint32_t budget_,carry_budget_;
    Graph prefix_,suffix_;
    ResidualPlan result_;
    std::array<std::uint16_t,max_program_nodes> load_map_{};
    std::unordered_map<std::uint64_t,std::pair<std::uint32_t,std::uint32_t>> atoms_;
    std::unordered_map<std::uint32_t,std::uint16_t> carries_;
    std::uint32_t carry(std::uint32_t prefix_id) {
        if(prefix_.is(prefix_id,false))return suffix_.constant(false);
        if(prefix_.is(prefix_id,true))return suffix_.constant(true);
        auto it=carries_.find(prefix_id);
        if(it==carries_.end()) {
            if(result_.carry_roots.size()>=carry_budget_)throw Failure{"residual carry budget exhausted; no partial plan is executable"};
            const auto id=static_cast<std::uint16_t>(result_.carry_roots.size());
            result_.carry_roots.push_back(prefix_id);it=carries_.emplace(prefix_id,id).first;
        }
        return suffix_.carry(it->second);
    }
    std::pair<std::uint32_t,std::uint32_t> atom_bounds(std::uint16_t load,std::int32_t threshold) {
        const auto key=(std::uint64_t(load)<<32)|std::bit_cast<std::uint32_t>(threshold);
        if(const auto it=atoms_.find(key);it!=atoms_.end())return it->second;
        PredicatePlan atom;atom.complete=true;atom.node_count=atom.load_count=atom.certificate_count=1;
        atom.nodes[0]={PredicateOp::Greater,0,0,0,threshold};
        atom.loads[0]=atom.certificates[0]=result_.exact.loads[load];
        const auto bounded=compile_frontier(atom,depth_,budget_);
        if(!bounded.complete)throw Failure{"residual atomic prefix lowering failed: "+bounded.reason};
        std::vector<std::uint32_t> remap(bounded.prefix.nodes.size());
        for(std::size_t i=0;i<remap.size();++i) {
            const auto& n=bounded.prefix.nodes[i];
            switch(n.op) {
            case WordcodeOp::False:case WordcodeOp::True:remap[i]=prefix_.constant(n.op==WordcodeOp::True);break;
            case WordcodeOp::Plane:
                if(n.bit>=depth_)throw Failure{"residual prefix reads a suffix plane"};
                remap[i]=prefix_.plane(load,n.bit);break;
            case WordcodeOp::And:case WordcodeOp::Or:remap[i]=prefix_.combine(convert(n.op),remap[n.a],remap[n.b]);break;
            case WordcodeOp::Not:remap[i]=prefix_.negate(remap[n.a]);break;
            }
        }
        const auto pair=std::pair{remap[bounded.prefix.must_root],remap[bounded.prefix.may_root]};
        atoms_.emplace(key,pair);return pair;
    }
    std::uint32_t suffix_compare(std::uint16_t load,std::int32_t threshold) {
        const auto k=std::bit_cast<std::uint32_t>(threshold)^0x80000000U;
        const auto& range=result_.exact.loads[load].range;
        const auto lo=static_cast<std::uint32_t>(range.lo)^0x80000000U;
        const auto hi=static_cast<std::uint32_t>(range.hi)^0x80000000U;
        auto g=suffix_.constant(false);
        // Least-significant-to-most-significant recurrence, restricted to the
        // suffix. A bit is constant only if the ENTIRE interval shares its
        // containing 2^(numeric_bit+1) prefix, not just endpoint bit values.
        for(unsigned bit=32;bit-->depth_;) {
            const auto numeric=31U-bit;
            const auto z=(lo>>numeric)==(hi>>numeric)?suffix_.constant(((lo>>numeric)&1U)!=0):
                suffix_.plane(load,static_cast<std::uint8_t>(bit));
            g=suffix_.combine(((k>>numeric)&1U)?ResidualOp::And:ResidualOp::Or,z,g);
        }
        return g;
    }
};

// Proof variables use DENSE identities of physical channels. A channel id is
// uint32, so channel*32 could wrap and falsely alias different inputs. The
// retained load count is bounded; first matching channel gives a safe dense
// identity while preserving aliases and shared comparisons of one channel.
std::uint32_t proof_variable(const ResidualPlan& p,std::uint16_t load,unsigned bit) {
    const auto channel=p.prefix.loads[load].channel;
    std::size_t identity=0;
    while(p.prefix.loads[identity].channel!=channel)++identity;
    return static_cast<std::uint32_t>(identity)*32+bit;
}
std::vector<BooleanId> proof_prefix(BooleanProof& proof,const ResidualPlan& p) {
    std::vector<BooleanId> values(p.prefix.nodes.size());
    for(std::size_t i=0;i<p.prefix.nodes.size();++i) {
        const auto& n=p.prefix.nodes[i];
        switch(n.op) {
        case WordcodeOp::False:values[i]=0;break;
        case WordcodeOp::True:values[i]=1;break;
        case WordcodeOp::Plane:values[i]=proof.variable(proof_variable(p,n.load_index,n.bit));break;
        case WordcodeOp::Not:values[i]=proof.negate(values[n.a]);break;
        case WordcodeOp::And:values[i]=proof.apply(BooleanOp::And,values[n.a],values[n.b]);break;
        case WordcodeOp::Or:values[i]=proof.apply(BooleanOp::Or,values[n.a],values[n.b]);break;
        }
    }
    return values;
}
BooleanId proof_suffix(BooleanProof& proof,const ResidualPlan& p,const ResidualStage& stage,
                       const std::vector<BooleanId>& prefix) {
    std::vector<BooleanId> values(stage.nodes.size());
    for(std::size_t i=0;i<stage.nodes.size();++i) {
        const auto& n=stage.nodes[i];
        switch(n.op) {
        case ResidualOp::False:values[i]=0;break;
        case ResidualOp::True:values[i]=1;break;
        case ResidualOp::Plane:values[i]=proof.variable(proof_variable(p,n.load_index,n.bit));break;
        case ResidualOp::Carry:values[i]=prefix[p.carry_roots[n.carry_index]];break;
        case ResidualOp::Not:values[i]=proof.negate(values[n.a]);break;
        case ResidualOp::And:values[i]=proof.apply(BooleanOp::And,values[n.a],values[n.b]);break;
        case ResidualOp::Or:values[i]=proof.apply(BooleanOp::Or,values[n.a],values[n.b]);break;
        }
    }
    return values[stage.root];
}
std::vector<std::uint16_t> active_carries(const ResidualStage& stage) {
    std::vector<std::uint16_t> result;
    for(const auto& n:stage.nodes)if(n.op==ResidualOp::Carry)result.push_back(n.carry_index);
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
enum class Replacement : std::uint8_t { False, True, Carry, ComplementCarry };
ResidualStage substitute_carry(const ResidualStage& stage,std::uint16_t target,
                               Replacement kind,std::uint16_t other,std::uint32_t budget) {
    Graph graph(budget);
    std::vector<std::uint32_t> mapped(stage.nodes.size());
    for(std::size_t i=0;i<stage.nodes.size();++i) {
        const auto& n=stage.nodes[i];
        switch(n.op) {
        case ResidualOp::False:case ResidualOp::True:mapped[i]=graph.constant(n.op==ResidualOp::True);break;
        case ResidualOp::Plane:mapped[i]=graph.plane(n.load_index,n.bit);break;
        case ResidualOp::Carry:
            if(n.carry_index!=target)mapped[i]=graph.carry(n.carry_index);
            else if(kind==Replacement::False || kind==Replacement::True)mapped[i]=graph.constant(kind==Replacement::True);
            else {
                mapped[i]=graph.carry(other);
                if(kind==Replacement::ComplementCarry)mapped[i]=graph.negate(mapped[i]);
            }
            break;
        case ResidualOp::Not:mapped[i]=graph.negate(mapped[n.a]);break;
        case ResidualOp::And:case ResidualOp::Or:mapped[i]=graph.combine(n.op,mapped[n.a],mapped[n.b]);break;
        }
    }
    std::vector<std::uint32_t> roots{mapped[stage.root]};graph.finish(roots);graph.result.root=roots[0];
    return std::move(graph.result);
}

void compact_guarded(ResidualPlan& p,std::uint32_t budget) {
    const auto active=active_carries(p.suffix);
    std::vector<std::uint16_t> carry_map(p.carry_roots.size());
    for(std::size_t i=0;i<active.size();++i)carry_map[active[i]]=static_cast<std::uint16_t>(i);
    for(auto& n:p.suffix.nodes)if(n.op==ResidualOp::Carry)n.carry_index=carry_map[n.carry_index];
    Graph prefix(budget);
    std::vector<std::uint32_t> mapped(p.prefix.nodes.size());
    for(std::size_t i=0;i<p.prefix.nodes.size();++i) {
        const auto& n=p.prefix.nodes[i];
        switch(n.op) {
        case WordcodeOp::False:case WordcodeOp::True:mapped[i]=prefix.constant(n.op==WordcodeOp::True);break;
        case WordcodeOp::Plane:mapped[i]=prefix.plane(n.load_index,n.bit);break;
        case WordcodeOp::Not:mapped[i]=prefix.negate(mapped[n.a]);break;
        case WordcodeOp::And:case WordcodeOp::Or:mapped[i]=prefix.combine(convert(n.op),mapped[n.a],mapped[n.b]);break;
        }
    }
    std::vector<std::uint32_t> roots{mapped[p.prefix.must_root],mapped[p.prefix.may_root]};
    for(const auto id:active)roots.push_back(mapped[p.carry_roots[id]]);
    prefix.finish(roots);
    p.prefix.must_root=roots[0];p.prefix.may_root=roots[1];
    p.carry_roots.assign(roots.begin()+2,roots.end());
    const auto& stage=prefix.result;
    p.prefix.nodes.clear();
    for(const auto& n:stage.nodes)p.prefix.nodes.push_back({convert(n.op),n.a,n.b,n.load_index,n.bit,n.slot});
    p.prefix.plane_count=stage.plane_count;p.prefix.operation_count=stage.operation_count;
    p.prefix.live_slot_count=stage.live_slot_count;p.prefix.maximum_depth=stage.maximum_depth;
}

void optimize_guarded(ResidualPlan& p,std::uint32_t graph_budget,GuardedResidualOptions options) {
    p.scope=ResidualScope::UnresolvedLanes;
    auto& stats=p.proof;stats.initial_carries=static_cast<std::uint32_t>(p.carry_roots.size());
    if(p.carry_roots.empty()) {stats.reason="no retained carry masks";return;}
    std::unique_ptr<BooleanProof> proof;
    try {
        if(!options.rewrite_attempt_budget)throw BooleanProofExhausted("rewrite attempt budget exhausted");
        proof=std::make_unique<BooleanProof>(options.bdd_node_budget,options.proof_step_budget);
        // Keep this original forest, reference residual, and care BDD unchanged
        // throughout the greedy search. Every accepted cumulative rewrite is
        // compared with the ORIGINAL residual under the ORIGINAL care set.
        const auto prefix=proof_prefix(*proof,p);
        const auto care=proof->apply(BooleanOp::And,prefix[p.prefix.may_root],proof->negate(prefix[p.prefix.must_root]));
        if(care==0) {
            ++stats.attempts;
            // The canonical false care function itself proves the miter is
            // false; constructing an unused full residual BDD would waste work.
            Graph zero(graph_budget);std::vector<std::uint32_t> roots{zero.constant(false)};
            zero.finish(roots);zero.result.root=roots[0];p.suffix=std::move(zero.result);
            ++stats.accepted_rewrites;stats.reason="empty care set; residual reduced to false";
        } else {
            const auto original=proof_suffix(*proof,p,p.suffix,prefix);
            bool progress=true;
            while(progress) {
                progress=false;
                const auto active=active_carries(p.suffix);
                if(active.empty())break;
                for(const auto target:active) {
                    std::vector<std::pair<Replacement,std::uint16_t>> candidates{
                        {Replacement::False,0},{Replacement::True,0}};
                    for(const auto other:active)if(other!=target) {
                        candidates.emplace_back(Replacement::Carry,other);
                        candidates.emplace_back(Replacement::ComplementCarry,other);
                    }
                    for(const auto& [kind,other]:candidates) {
                        if(stats.attempts>=options.rewrite_attempt_budget)
                            throw BooleanProofExhausted("rewrite attempt budget exhausted");
                        ++stats.attempts;
                        ResidualStage candidate;
                        try {candidate=substitute_carry(p.suffix,target,kind,other,graph_budget);}
                        catch(const Failure&) {++stats.graph_budget_rejections;continue;}
                        if(active_carries(candidate).size()>=active.size())continue;
                        const auto value=proof_suffix(*proof,p,candidate,prefix);
                        if(!proof->equivalent_under(care,original,value))continue;
                        p.suffix=std::move(candidate);++stats.accepted_rewrites;
                        progress=true;break;
                    }
                    if(progress)break;
                }
            }
            stats.reason="bounded carry substitution search completed";
        }
    } catch(const BooleanProofExhausted& exhausted) {
        stats.exhausted=true;stats.reason=exhausted.what();
    }
    if(proof) {stats.bdd_nodes=proof->node_count();stats.proof_steps=proof->steps();}
    // No new prefix expressions are introduced here, so compaction cannot
    // exceed the already successful graph budget. Certificates stay untouched.
    compact_guarded(p,graph_budget);
}

bool same_loads(const std::vector<Load>& a,const std::vector<Load>& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)
        if(a[i].channel!=b[i].channel || a[i].offset!=b[i].offset || a[i].range!=b[i].range)return false;
    return true;
}
bool same_nodes(const std::vector<WordcodeNode>& a,const std::vector<WordcodeNode>& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        const auto& x=a[i];const auto& y=b[i];
        if(x.op!=y.op || x.a!=y.a || x.b!=y.b || x.load_index!=y.load_index || x.bit!=y.bit || x.slot!=y.slot)return false;
    }
    return true;
}
bool same_prefix(const FrontierStage& a,const FrontierStage& b) {
    return a.must_root==b.must_root && a.may_root==b.may_root && a.plane_count==b.plane_count &&
        a.operation_count==b.operation_count && a.live_slot_count==b.live_slot_count &&
        a.maximum_depth==b.maximum_depth && same_loads(a.loads,b.loads) && same_nodes(a.nodes,b.nodes);
}
bool same_exact(const WordcodePlan& a,const WordcodePlan& b) {
    return a.complete==b.complete && a.root==b.root && a.plane_count==b.plane_count &&
        a.operation_count==b.operation_count && a.live_slot_count==b.live_slot_count &&
        a.maximum_depth==b.maximum_depth && a.source_predicate_nodes==b.source_predicate_nodes &&
        a.source_comparisons==b.source_comparisons && same_loads(a.loads,b.loads) && same_nodes(a.nodes,b.nodes);
}
bool same_suffix(const ResidualStage& a,const ResidualStage& b) {
    return a.nodes==b.nodes && a.root==b.root && a.plane_count==b.plane_count &&
        a.operation_count==b.operation_count && a.live_slot_count==b.live_slot_count && a.maximum_depth==b.maximum_depth;
}
void check_boundary(const ResidualPlan& p) {
    for(const auto& n:p.prefix.nodes)if(n.op==WordcodeOp::Plane && n.bit>=p.prefix_bits)
        throw std::invalid_argument("residual prefix reads beyond its prefix depth");
    for(const auto& n:p.suffix.nodes) {
        if(n.op==ResidualOp::Plane && (n.bit<p.prefix_bits || n.bit>=32 || n.load_index>=p.prefix.loads.size()))
            throw std::invalid_argument("residual stage reads outside its certified suffix");
        if(n.op==ResidualOp::Carry && n.carry_index>=p.carry_roots.size())
            throw std::invalid_argument("residual stage references an undeclared carry");
    }
    for(const auto root:p.carry_roots)if(root>=p.prefix.nodes.size())
        throw std::invalid_argument("residual carry root is outside its prefix forest");
}
} // namespace

ResidualPlan compile_residual(const PredicatePlan& source,unsigned depth,std::uint32_t budget,std::uint32_t carry_budget) {
    if(depth>32)throw std::invalid_argument("residual prefix_bits must be in 0..32");
    if(!budget || budget>max_wordcode_nodes)throw std::invalid_argument("residual node budget must be in 1..16384");
    if(carry_budget>max_residual_carries)throw std::invalid_argument("residual carry budget exceeds 512");
    try {
        auto exact=compile_wordcode(source,budget);
        if(!exact.complete)throw Failure{"residual exact lowering failed: "+exact.reason};
        auto result=Compiler(source,depth,budget,carry_budget,std::move(exact)).compile();
        result.source_=source;result.node_budget_=budget;result.carry_budget_=carry_budget;
        check_boundary(result);return result;
    } catch(const Failure& failure) {
        ResidualPlan result;result.prefix_bits=depth;result.reason=failure.reason;return result;
    }
}
ResidualPlan compile_guarded_residual(const PredicatePlan& source,unsigned depth,std::uint32_t budget,
                                      std::uint32_t carry_budget,GuardedResidualOptions options) {
    if(options.bdd_node_budget>262144 || options.proof_step_budget>8000000 || options.rewrite_attempt_budget>4096)
        throw std::invalid_argument("guarded residual proof options exceed their hard maxima");
    auto result=compile_residual(source,depth,budget,carry_budget);
    if(!result.complete)return result;
    // Retain a complete baseline until optimization returns. Non-budget
    // construction failure also fails closed to this all-lane executable.
    auto candidate=result;
    try {optimize_guarded(candidate,budget,options);}
    catch(const Failure& failure) {
        result.scope=ResidualScope::UnresolvedLanes;
        result.proof.initial_carries=static_cast<std::uint32_t>(result.carry_roots.size());
        result.proof.exhausted=true;result.proof.reason="guarded construction fallback: "+failure.reason;
        candidate=std::move(result);
    }
    candidate.guarded_=true;candidate.guarded_options_=options;
    check_boundary(candidate);return candidate;
}
void validate_residual(const ResidualPlan& p) {
    if(!p.complete || !p.exact.complete || p.prefix_bits>32 || !p.node_budget_ ||
       p.node_budget_>max_wordcode_nodes || p.carry_budget_>max_residual_carries ||
       p.prefix.nodes.empty() || p.suffix.nodes.empty() || p.exact.nodes.empty() ||
       p.prefix.nodes.size()>p.node_budget_ || p.suffix.nodes.size()>p.node_budget_ ||
       p.exact.nodes.size()>p.node_budget_ || p.carry_roots.size()>p.carry_budget_ ||
       p.prefix.loads.size()>max_program_nodes || p.exact.loads.size()>max_program_nodes)
        throw std::invalid_argument("residual plan is incomplete, unconstructed, or exceeds its retained budget");
    check_boundary(p);
    const auto canonical=p.guarded_?compile_guarded_residual(p.source_,p.prefix_bits,p.node_budget_,p.carry_budget_,p.guarded_options_):
        compile_residual(p.source_,p.prefix_bits,p.node_budget_,p.carry_budget_);
    if(!canonical.complete || p.carry_roots!=canonical.carry_roots || !same_prefix(p.prefix,canonical.prefix) ||
       !same_suffix(p.suffix,canonical.suffix) || !same_exact(p.exact,canonical.exact) ||
       p.scope!=canonical.scope || p.proof!=canonical.proof)
        throw std::invalid_argument("residual plan differs from its canonical source witness");
}
std::string ResidualPlan::describe() const {
    if(!complete)return "residual fallback: "+reason;
    std::ostringstream out;
    out<<(scope==ResidualScope::AllLanes?"exact suffix residual":"exact-on-care suffix residual")
       <<"; prefix_bits="<<prefix_bits<<"; prefix_nodes="<<prefix.nodes.size()
       <<"; prefix_planes="<<prefix.plane_count<<"; prefix_slots="<<prefix.live_slot_count
       <<"; carry_masks="<<carry_roots.size()<<"; suffix_nodes="<<suffix.nodes.size()
       <<"; suffix_planes="<<suffix.plane_count<<"; suffix_slots="<<suffix.live_slot_count;
    if(scope==ResidualScope::UnresolvedLanes)out<<"; initial_carries="<<proof.initial_carries
        <<"; admitted_rewrites="<<proof.accepted_rewrites<<"; proof_attempts="<<proof.attempts
        <<"; bdd_nodes="<<proof.bdd_nodes<<"; proof_steps="<<proof.proof_steps
        <<"; proof_exhausted="<<proof.exhausted;
    return out.str();
}
} // namespace suffice
