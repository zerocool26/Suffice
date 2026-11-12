#include "suffice/boolean_proof.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace suffice {
struct BooleanProof::Impl {
    static constexpr auto terminal_variable = std::numeric_limits<std::uint32_t>::max();
    static constexpr unsigned maximum_apply_depth = 1024;
    struct Node {
        std::uint32_t variable{};
        BooleanId low{}, high{};
        friend bool operator==(const Node&, const Node&) = default;
    };
    struct Key {
        BooleanOp op{};
        BooleanId a{}, b{};
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct NodeHash {
        std::size_t operator()(const Node& n) const noexcept {
            return static_cast<std::size_t>((std::uint64_t(n.variable)*0x9e3779b185ebca87ULL) ^
                (std::uint64_t(n.low)*0xc2b2ae3d27d4eb4fULL) ^ (std::uint64_t(n.high)*0x165667b19e3779f9ULL));
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return static_cast<std::size_t>((std::uint64_t(k.a)*0x9e3779b185ebca87ULL) ^
                (std::uint64_t(k.b)*0xc2b2ae3d27d4eb4fULL) ^ static_cast<unsigned>(k.op));
        }
    };
    std::uint32_t node_budget;
    std::uint64_t step_budget, used_steps{};
    std::vector<Node> nodes;
    std::unordered_map<Node,BooleanId,NodeHash> unique;
    std::unordered_map<Key,BooleanId,KeyHash> cache;

    Impl(std::uint32_t n, std::uint64_t s):node_budget(n),step_budget(s) {
        if(n<2)throw BooleanProofExhausted("BDD node budget cannot hold both terminals");
        nodes.push_back({terminal_variable,0,0});
        nodes.push_back({terminal_variable,1,1});
    }
    void charge() {
        if(used_steps>=step_budget)throw BooleanProofExhausted("BDD proof step budget exhausted");
        ++used_steps;
    }
    void check(BooleanId id) const {
        if(id>=nodes.size())throw std::invalid_argument("Boolean proof node is outside its manager");
    }
    BooleanId make(std::uint32_t variable,BooleanId low,BooleanId high) {
        charge();
        if(low==high)return low;
        const Node node{variable,low,high};
        if(const auto it=unique.find(node);it!=unique.end())return it->second;
        if(nodes.size()>=node_budget)throw BooleanProofExhausted("BDD node budget exhausted");
        const auto id=static_cast<BooleanId>(nodes.size());
        nodes.push_back(node);unique.emplace(node,id);return id;
    }
    BooleanId apply(BooleanOp op,BooleanId a,BooleanId b,unsigned depth=0) {
        charge();
        if(depth>maximum_apply_depth)throw BooleanProofExhausted("BDD apply depth limit exhausted");
        if(a>b)std::swap(a,b);
        if(a==b)return op==BooleanOp::Xor?0:a;
        if(op==BooleanOp::And) {
            if(a==0)return 0;
            if(a==1)return b;
        } else if(op==BooleanOp::Or) {
            if(a==0)return b;
            if(a==1 || b==1)return 1;
        } else if(a==0)return b;
        if(a<2 && b<2) {
            return op==BooleanOp::And?(a&b):op==BooleanOp::Or?(a|b):(a^b);
        }
        const Key key{op,a,b};
        if(const auto it=cache.find(key);it!=cache.end())return it->second;
        const auto variable=std::min(nodes[a].variable,nodes[b].variable);
        const auto al=nodes[a].variable==variable?nodes[a].low:a;
        const auto ah=nodes[a].variable==variable?nodes[a].high:a;
        const auto bl=nodes[b].variable==variable?nodes[b].low:b;
        const auto bh=nodes[b].variable==variable?nodes[b].high:b;
        const auto low=apply(op,al,bl,depth+1);
        const auto high=apply(op,ah,bh,depth+1);
        const auto result=make(variable,low,high);
        // Bounded memo storage. Skipping cache inserts changes only cost.
        if(cache.size()<std::uint64_t(node_budget)*4)cache.emplace(key,result);
        return result;
    }
};

BooleanProof::BooleanProof(std::uint32_t nodes,std::uint64_t steps):impl_(std::make_unique<Impl>(nodes,steps)) {}
BooleanProof::~BooleanProof()=default;
BooleanProof::BooleanProof(BooleanProof&&) noexcept=default;
BooleanProof& BooleanProof::operator=(BooleanProof&&) noexcept=default;
BooleanId BooleanProof::variable(std::uint32_t index) {
    if(index==Impl::terminal_variable)throw std::invalid_argument("reserved Boolean variable identifier");
    return impl_->make(index,0,1);
}
BooleanId BooleanProof::apply(BooleanOp op,BooleanId a,BooleanId b) {
    if(op!=BooleanOp::And && op!=BooleanOp::Or && op!=BooleanOp::Xor)
        throw std::invalid_argument("invalid Boolean proof operation");
    impl_->check(a);impl_->check(b);return impl_->apply(op,a,b);
}
BooleanId BooleanProof::negate(BooleanId value) {return apply(BooleanOp::Xor,value,1);}
bool BooleanProof::equivalent_under(BooleanId care,BooleanId left,BooleanId right) {
    impl_->check(care);impl_->check(left);impl_->check(right);
    return apply(BooleanOp::And,care,apply(BooleanOp::Xor,left,right))==0;
}
bool BooleanProof::evaluate(BooleanId value,std::span<const std::uint8_t> assignment) const {
    impl_->check(value);
    while(value>=2) {
        const auto& node=impl_->nodes[value];
        if(node.variable>=assignment.size())throw std::invalid_argument("Boolean proof assignment is incomplete");
        value=assignment[node.variable]?node.high:node.low;
    }
    return value!=0;
}
std::uint32_t BooleanProof::node_count() const noexcept {return static_cast<std::uint32_t>(impl_->nodes.size());}
std::uint64_t BooleanProof::steps() const noexcept {return impl_->used_steps;}
} // namespace suffice
