#include "suffice/residual.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace suffice;
constexpr auto minimum=std::numeric_limits<std::int32_t>::min();
constexpr auto maximum=std::numeric_limits<std::int32_t>::max();
void require(bool test,const char* message) {
    if(!test)throw std::runtime_error(std::string("residual test: ")+message);
}
template<class F> void rejects(F&& f,const char* message) {
    bool caught=false;try {f();} catch(const std::invalid_argument&) {caught=true;}require(caught,message);
}
std::uint64_t random(std::uint64_t& s) {
    s+=0x9e3779b97f4a7c15ULL;auto z=s;z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27))*0x94d049bb133111ebULL;return z^(z>>31);
}
PredicateNodeId add(PredicatePlan& p,PredicateOp op,PredicateNodeId a=0,PredicateNodeId b=0,
                    std::uint16_t load=0,std::int32_t threshold=0) {
    const auto id=p.node_count++;require(id<max_predicate_nodes,"fixture exceeds source bound");
    p.nodes[id]={op,a,b,load,threshold};p.root=id;return id;
}
PredicatePlan source(unsigned channels,Bounds range={minimum,maximum}) {
    PredicatePlan p;p.complete=true;p.load_count=p.certificate_count=static_cast<std::uint16_t>(channels);
    for(std::uint16_t i=0;i<channels;++i)p.loads[i]=p.certificates[i]={i,0,range};
    return p;
}
PredicatePlan atom(std::int32_t t,Bounds range={minimum,maximum}) {
    auto p=source(1,range);add(p,PredicateOp::Greater,0,0,0,t);return p;
}
bool direct(const PredicatePlan& p,std::span<const std::int32_t> row) {
    std::array<bool,max_predicate_nodes> v{};
    for(std::size_t i=0;i<p.node_count;++i) {
        const auto& n=p.nodes[i];
        switch(n.op) {
        case PredicateOp::False:v[i]=false;break;
        case PredicateOp::True:v[i]=true;break;
        case PredicateOp::Greater:v[i]=row[p.loads[n.load_index].channel]>n.threshold;break;
        case PredicateOp::And:v[i]=v[n.a]&&v[n.b];break;
        case PredicateOp::Or:v[i]=v[n.a]||v[n.b];break;
        case PredicateOp::Not:v[i]=!v[n.a];break;
        }
    }
    return v[p.root];
}
std::uint64_t plane(std::span<const std::int32_t> values,unsigned channels,unsigned channel,unsigned bit) {
    std::uint64_t v=0;
    for(std::size_t i=0;i<values.size()/channels;++i) {
        // Numeric division is deliberately independent of the compiler's
        // threshold recurrence and the backend's packed-plane reader.
        const auto ordered=std::uint64_t(std::int64_t(values[i*channels+channel])-minimum);
        v|=((ordered/(std::uint64_t{1}<<(31-bit)))%2)<<i;
    }
    return v;
}
struct Outcome {std::uint64_t must{},may{},exact{};};
Outcome check(const PredicatePlan& source,const ResidualPlan& p,std::span<const std::int32_t> values,
              unsigned channels,std::uint64_t active=UINT64_MAX,bool arenas=false) {
    require(p.complete,"valid residual failed bounded compilation");
    const auto rows=values.size()/channels;require(rows<=64,"fixture word too wide");
    const auto valid=(rows==64?UINT64_MAX:((std::uint64_t{1}<<rows)-1))&active;
    std::uint64_t wanted=0;
    for(std::size_t i=0;i<rows;++i)if(direct(source,values.subspan(i*channels,channels)))wanted|=std::uint64_t{1}<<i;
    wanted&=valid;
    std::vector<std::uint64_t> prefix(p.prefix.nodes.size());
    for(std::size_t i=0;i<p.prefix.nodes.size();++i) {
        const auto& n=p.prefix.nodes[i];
        switch(n.op) {
        case WordcodeOp::False:prefix[i]=0;break;
        case WordcodeOp::True:prefix[i]=valid;break;
        case WordcodeOp::Plane:
            require(n.bit<p.prefix_bits,"prefix queried a suffix plane");
            prefix[i]=plane(values,channels,p.prefix.loads[n.load_index].channel,n.bit)&valid;break;
        case WordcodeOp::And:prefix[i]=prefix[n.a]&prefix[n.b];break;
        case WordcodeOp::Or:prefix[i]=prefix[n.a]|prefix[n.b];break;
        case WordcodeOp::Not:prefix[i]=~prefix[n.a]&valid;break;
        }
    }
    std::vector<std::uint64_t> carried;for(const auto id:p.carry_roots)carried.push_back(prefix[id]);
    std::vector<std::uint64_t> suffix(p.suffix.nodes.size());
    for(std::size_t i=0;i<p.suffix.nodes.size();++i) {
        const auto& n=p.suffix.nodes[i];
        switch(n.op) {
        case ResidualOp::False:suffix[i]=0;break;
        case ResidualOp::True:suffix[i]=valid;break;
        case ResidualOp::Plane:
            // The independent suffix reader refuses every prefix payload read.
            require(n.bit>=p.prefix_bits && n.bit<32,"residual queried prefix payload");
            suffix[i]=plane(values,channels,p.prefix.loads[n.load_index].channel,n.bit)&valid;break;
        case ResidualOp::Carry:require(n.carry_index<carried.size(),"unknown carry");suffix[i]=carried[n.carry_index];break;
        case ResidualOp::And:suffix[i]=suffix[n.a]&suffix[n.b];break;
        case ResidualOp::Or:suffix[i]=suffix[n.a]|suffix[n.b];break;
        case ResidualOp::Not:suffix[i]=~suffix[n.a]&valid;break;
        }
    }
    const auto lower=prefix[p.prefix.must_root],upper=prefix[p.prefix.may_root],exact=suffix[p.suffix.root];
    if(p.scope==ResidualScope::AllLanes)
        require(exact==wanted,"suffix plus carries differs from direct signed comparisons");
    else require(((exact^wanted)&upper&~lower)==0,"guarded suffix differs on unresolved lanes");
    require((lower&~wanted)==0 && (wanted&~upper)==0 && (lower&~upper)==0,"prefix bounds unsound");
    require((lower|((upper&~lower)&exact))==wanted,"staged merge differs");
    if(p.prefix_bits==32)require(lower==wanted && upper==wanted,"full depth leaves uncertainty");
    if(arenas) {
        std::vector<std::uint64_t> arena(p.prefix.live_slot_count,0x6996966996696996ULL);
        const auto get=[&](std::uint32_t id) {const auto slot=p.prefix.nodes[id].slot;return slot==no_wordcode_slot?prefix[id]:arena[slot];};
        for(const auto& n:p.prefix.nodes)if(n.slot!=no_wordcode_slot) {
            require(n.slot<arena.size(),"prefix slot exceeds arena");
            const auto a=get(n.a);const auto b=n.op==WordcodeOp::Not?0:get(n.b);
            arena[n.slot]=n.op==WordcodeOp::Not?(~a&valid):n.op==WordcodeOp::And?(a&b):(a|b);
        }
        for(const auto id:p.carry_roots)require(get(id)==prefix[id],"a retained prefix carry was overwritten");
        require(get(p.prefix.must_root)==lower && get(p.prefix.may_root)==upper,"bounds overwritten by carry work");
        std::vector<std::uint64_t> work(p.suffix.live_slot_count,0x9696699669969669ULL);
        const auto value=[&](std::uint32_t id) {const auto slot=p.suffix.nodes[id].slot;return slot==no_wordcode_slot?suffix[id]:work[slot];};
        for(const auto& n:p.suffix.nodes)if(n.slot!=no_wordcode_slot) {
            require(n.slot<work.size(),"suffix slot exceeds arena");
            const auto a=value(n.a);const auto b=n.op==ResidualOp::Not?0:value(n.b);
            work[n.slot]=n.op==ResidualOp::Not?(~a&valid):n.op==ResidualOp::And?(a&b):(a|b);
        }
        require(value(p.suffix.root)==exact,"suffix slot reuse lost result");
    }
    return {lower,upper,exact};
}

void exhaustive_atoms() {
    // Every input, threshold, and depth for widths 1..8. The n-bit unsigned
    // domain is embedded at the low end of order-biased int32. All other high
    // planes are certificate constants, so production code sees the same cut.
    for(unsigned width=1;width<=8;++width) {
        const unsigned size=1U<<width;
        std::vector<std::int32_t> values(size);
        for(unsigned u=0;u<size;++u)values[u]=minimum+static_cast<std::int32_t>(u);
        for(unsigned k=0;k<size;++k)for(unsigned d=0;d<=width;++d) {
            const auto source=atom(minimum+static_cast<std::int32_t>(k),{minimum,minimum+std::int64_t(size)-1});
            const auto p=compile_residual(source,32-width+d);
            for(unsigned first=0;first<size;first+=64)
                check(source,p,std::span(values).subspan(first,std::min<unsigned>(64,size-first)),1,UINT64_MAX,first==0);
            const auto cell=1U<<(width-d);
            for(unsigned u=0;u<size;++u) {
                const bool g=u/cell>k/cell,e=u/cell==k/cell,s=u%cell>k%cell;
                require(!(g&&e) && (g||(e&&s))==(u>k),"independent integer cofactor identity failed");
            }
        }
    }
}
void compound_and_masks() {
    std::vector<std::int32_t> values;
    for(int a=0;a<8;++a)for(int b=0;b<8;++b)for(int c=0;c<8;++c)
        values.insert(values.end(),{minimum+a,minimum+b,minimum+c});
    for(int t0=0;t0<8;++t0)for(int t1=0;t1<8;++t1) {
        auto p=source(3,{minimum,minimum+7});
        const auto a=add(p,PredicateOp::Greater,0,0,0,minimum+t0);
        const auto b=add(p,PredicateOp::Greater,0,0,1,minimum+t1);
        const auto c=add(p,PredicateOp::Greater,0,0,0,minimum+t1); // correlated with a
        const auto e=add(p,PredicateOp::Greater,0,0,2,minimum+3);
        const auto ab=add(p,PredicateOp::Or,a,b);
        const auto na=add(p,PredicateOp::Not,a);
        const auto ace=add(p,PredicateOp::Or,na,add(p,PredicateOp::And,c,e));
        add(p,PredicateOp::And,ab,ace);
        for(unsigned depth=29;depth<=32;++depth) {
            const auto residual=compile_residual(p,depth);
            for(unsigned first=0;first<512;first+=64)
                check(p,residual,std::span(values).subspan(first*3,64*3),3,UINT64_MAX,true);
            if(t0==3 && t1==5)for(unsigned mask=0;mask<256;++mask)
                check(p,residual,std::span(values).subspan(190*3,8*3),3,mask,true);
        }
    }
    // Same suffix and root bounds, different result. The per-atom evidence is
    // indispensable: (4,10) gives false; (8,6) gives true at a 2/2-bit cut.
    auto p=source(2,{minimum,minimum+15});
    const auto a=add(p,PredicateOp::Greater,0,0,0,minimum+5);
    const auto b=add(p,PredicateOp::Greater,0,0,1,minimum+5);add(p,PredicateOp::And,a,b);
    const auto r=compile_residual(p,30);
    const std::array<std::int32_t,4> counterexample{minimum+4,minimum+10,minimum+8,minimum+6};
    const auto result=check(p,r,counterexample,2,UINT64_MAX,true);
    require(result.must==0 && result.may==3 && result.exact==2,"root-only carry counterexample lost");
    require(r.carry_roots.size()>=2,"insufficient atom evidence retained");
}
void full_width_and_random() {
    std::uint64_t seed=0x524553494455414cULL;
    const std::array<std::int32_t,13> thresholds{minimum,minimum+1,minimum+15,-65537,-4097,-1,0,1,4095,65535,maximum-2,maximum-1,maximum};
    for(const auto t:thresholds) {
        const auto p=atom(t);std::array<std::int32_t,64> values{};
        for(auto& v:values)v=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random(seed)));
        values[0]=minimum;values[1]=maximum;values[2]=t;values[3]=t==minimum?t:t-1;values[4]=t==maximum?t:t+1;
        for(unsigned d=0;d<=32;++d) {
            const auto r=compile_residual(p,d);validate_residual(r);check(p,r,values,1,UINT64_MAX,true);
            check(p,r,std::span(values).subspan(3,17),1,0xaaaau,true);
        }
    }
    for(unsigned fixture=0;fixture<40;++fixture) {
        auto p=source(3);
        for(unsigned i=0;i<9;++i)add(p,PredicateOp::Greater,0,0,i%3,
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random(seed))));
        for(unsigned i=0;i<30;++i) {
            const auto a=static_cast<PredicateNodeId>(random(seed)%p.node_count);
            const auto b=static_cast<PredicateNodeId>(random(seed)%p.node_count);
            add(p,i%3==0?PredicateOp::Not:i%3==1?PredicateOp::And:PredicateOp::Or,a,b);
        }
        std::array<std::int32_t,64*3> values{};
        for(auto& v:values)v=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random(seed)));
        for(unsigned d=0;d<=32;++d)check(p,compile_residual(p,d),values,3,UINT64_MAX,true);
    }
}
void witness_and_budgets() {
    const auto source=atom(1234567);
    const auto good=compile_residual(source,12);validate_residual(good);
    auto bad=good;bad.suffix.nodes[bad.suffix.root].a^=1;
    rejects([&]{validate_residual(bad);},"modified suffix accepted");
    bad=good;bad.carry_roots[0]=bad.prefix.must_root==bad.carry_roots[0]?bad.prefix.may_root:bad.prefix.must_root;
    rejects([&]{validate_residual(bad);},"modified carry accepted");
    bad=good;bad.prefix.loads[0].range.lo=0;
    rejects([&]{validate_residual(bad);},"modified certificate accepted");
    bad=good;for(auto& n:bad.suffix.nodes)if(n.op==ResidualOp::Plane){n.bit=0;break;}
    rejects([&]{validate_residual(bad);},"prefix-read suffix accepted");
    bad=good;for(auto& n:bad.suffix.nodes)if(n.op==ResidualOp::Carry){n.carry_index=static_cast<std::uint16_t>(bad.carry_roots.size());break;}
    rejects([&]{validate_residual(bad);},"undeclared carry accepted");
    bad=good;bad.prefix.live_slot_count+=1;
    rejects([&]{validate_residual(bad);},"modified liveness accepted");
    rejects([&]{validate_residual(ResidualPlan{});},"unconstructed plan accepted");
    rejects([&]{(void)compile_residual(source,33);},"depth 33 accepted");
    rejects([&]{(void)compile_residual(source,12,0);},"zero graph budget accepted");
    rejects([&]{(void)compile_residual(source,12,4096,max_residual_carries+1);},"unbounded carry budget accepted");
    for(const auto& r:{compile_residual(source,12,1),compile_residual(source,12,4096,0)}) {
        require(!r.complete && r.prefix.nodes.empty() && r.suffix.nodes.empty() && r.exact.nodes.empty(),"failed compilation leaked a partial graph");
    }
    const auto folded=compile_residual(atom(10,{20,30}),12,4096,0);
    require(folded.complete && folded.carry_roots.empty() && folded.suffix.plane_count==0 &&
        folded.prefix.loads.size()==1 && folded.prefix.loads[0].range==Bounds{20,30},"proof-only source certificate lost");
    auto unsupported=source;unsupported.loads[0].offset=1;
    require(!compile_residual(unsupported).complete,"nonzero live source offset accepted");
}

void guarded_small_domains() {
    // Distinct and aliased inputs, mixed AND/OR/NOT, and shared subexpressions.
    // Every assignment in a three-channel 3-bit domain, at every local cut.
    std::vector<std::int32_t> values;
    for(int a=0;a<8;++a)for(int b=0;b<8;++b)for(int c=0;c<8;++c)
        values.insert(values.end(),{minimum+a,minimum+b,minimum+c});
    std::uint64_t admitted=0;
    for(unsigned fixture=0;fixture<12;++fixture) {
        auto p=source(3,{minimum,minimum+7});
        const auto a=add(p,PredicateOp::Greater,0,0,0,minimum+int(fixture%7));
        const auto b=add(p,PredicateOp::Greater,0,0,1,minimum+int((fixture*3)%7));
        const auto c=add(p,PredicateOp::Greater,0,0,2,minimum+3);
        const auto same=add(p,PredicateOp::Greater,0,0,0,minimum+5);
        if(fixture%4==0)add(p,PredicateOp::And,add(p,PredicateOp::And,a,b),c);
        else if(fixture%4==1)add(p,PredicateOp::Or,add(p,PredicateOp::Or,a,b),c);
        else if(fixture%4==2) {
            const auto shared=add(p,PredicateOp::Or,a,b);
            add(p,PredicateOp::And,shared,add(p,PredicateOp::Or,add(p,PredicateOp::Not,a),add(p,PredicateOp::And,c,same)));
        } else add(p,PredicateOp::Not,add(p,PredicateOp::And,add(p,PredicateOp::Or,a,b),add(p,PredicateOp::Not,c)));
        for(unsigned d=29;d<=32;++d) {
            const auto guarded=compile_guarded_residual(p,d);validate_residual(guarded);
            require(guarded.scope==ResidualScope::UnresolvedLanes,"guarded scope not retained");
            require(guarded.carry_roots.size()<=guarded.proof.initial_carries,"optimizer added carry state");
            require(guarded.proof.attempts<=256 && guarded.proof.bdd_nodes<=65536 && guarded.proof.proof_steps<=1000000,"optimizer exceeded budgets");
            admitted+=guarded.proof.accepted_rewrites;
            for(unsigned first=0;first<512;first+=64)
                check(p,guarded,std::span(values).subspan(first*3,64*3),3,UINT64_MAX,true);
            check(p,guarded,std::span(values).subspan(190*3,8*3),3,0xa5,true);
        }
    }
    require(admitted>20,"finite-domain fixtures failed to exercise admitted cumulative rewrites");
}

void guarded_full_width_aliases_and_ids() {
    std::uint64_t seed=0x4755415244454436ULL;
    for(unsigned fixture=0;fixture<8;++fixture) {
        auto p=source(3);
        for(unsigned i=0;i<6;++i)add(p,PredicateOp::Greater,0,0,i%3,
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random(seed))));
        for(unsigned i=0;i<12;++i) {
            const auto a=static_cast<PredicateNodeId>(random(seed)%p.node_count);
            const auto b=static_cast<PredicateNodeId>(random(seed)%p.node_count);
            add(p,i%3==0?PredicateOp::Not:i%3==1?PredicateOp::And:PredicateOp::Or,a,b);
        }
        std::array<std::int32_t,64*3> values{};
        for(auto& value:values)value=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(random(seed)));
        for(unsigned i=0;i<6;++i) {
            const auto t=p.nodes[i].threshold;
            values[(i*3)*3+i%3]=t;
            values[(i*3+1)*3+i%3]=t==minimum?t:t-1;
            values[(i*3+2)*3+i%3]=t==maximum?t:t+1;
        }
        for(unsigned d=0;d<=32;++d) {
            const auto guarded=compile_guarded_residual(p,d);
            check(p,guarded,values,3,UINT64_MAX,true);
            if(d==0 || d==12 || d==32)validate_residual(guarded);
        }
    }
    auto aliases=source(2,{minimum,minimum+15});
    aliases.load_count=3;aliases.loads[2]=aliases.loads[1];aliases.loads[1]=aliases.loads[0];
    const auto aa=add(aliases,PredicateOp::Greater,0,0,0,minimum+5);
    const auto ab=add(aliases,PredicateOp::Greater,0,0,1,minimum+9);
    const auto ac=add(aliases,PredicateOp::Greater,0,0,2,minimum+5);
    add(aliases,PredicateOp::And,add(aliases,PredicateOp::Or,aa,ac),add(aliases,PredicateOp::Not,ab));
    std::vector<std::int32_t> all;
    for(int a=0;a<16;++a)for(int b=0;b<16;++b)all.insert(all.end(),{minimum+a,minimum+b});
    for(unsigned d=28;d<=32;++d) {
        const auto g=compile_guarded_residual(aliases,d);validate_residual(g);
        require(g.prefix.loads.size()==2,"physical alias certificate not canonicalized");
        for(unsigned first=0;first<256;first+=64)check(aliases,g,std::span(all).subspan(first*2,128),2,UINT64_MAX,true);
    }
    // channel*32 would overflow here and incorrectly identify two independent
    // channels. Dense proof variable identities must make renaming harmless.
    auto low=source(2,{minimum,minimum+15});
    const auto a=add(low,PredicateOp::Greater,0,0,0,minimum+5);
    const auto b=add(low,PredicateOp::Greater,0,0,1,minimum+9);add(low,PredicateOp::And,a,b);
    auto high=low;high.loads[1].channel=high.certificates[1].channel=1U<<27;
    for(unsigned d=28;d<=32;++d) {
        const auto expected=compile_guarded_residual(low,d);
        const auto actual=compile_guarded_residual(high,d);validate_residual(actual);
        require(actual.proof==expected.proof && actual.carry_roots.size()==expected.carry_roots.size(),"high channel id changed Boolean variable identity");
        auto renamed=actual;
        for(auto& load:renamed.prefix.loads)if(load.channel==(1U<<27))load.channel=1;
        for(unsigned first=0;first<256;first+=64)check(low,renamed,std::span(all).subspan(first*2,128),2,UINT64_MAX,true);
    }
}

void guarded_limits_and_witness() {
    auto p=source(2);
    const auto a=add(p,PredicateOp::Greater,0,0,0,1234567);
    const auto b=add(p,PredicateOp::Greater,0,0,1,-234567);add(p,PredicateOp::And,a,b);
    const auto original=compile_residual(p,12),good=compile_guarded_residual(p,12);
    validate_residual(good);
    require(good.proof.accepted_rewrites>=2 && good.carry_roots.size()<original.carry_roots.size(),"pure conjunction failed to reduce evidence");
    const std::array<std::int32_t,12> values{minimum,minimum,maximum,maximum,1234567,-234566,1234568,-234568,1234568,-234566,0,0};
    check(p,good,values,2,UINT64_MAX,true);
    for(const auto options:{GuardedResidualOptions{0,1000000,256},GuardedResidualOptions{1,1000000,256},
                           GuardedResidualOptions{2,1000000,256},GuardedResidualOptions{65536,0,256},
                           GuardedResidualOptions{65536,1000000,0},GuardedResidualOptions{65536,1000000,1}}) {
        const auto g=compile_guarded_residual(p,12,4096,512,options);validate_residual(g);
        require(g.complete && g.proof.exhausted,"exhausted proof did not retain executable fallback");
        require(g.proof.attempts<=options.rewrite_attempt_budget && g.proof.bdd_nodes<=options.bdd_node_budget && g.proof.proof_steps<=options.proof_step_budget,"fallback exceeded proof budget");
        check(p,g,values,2,UINT64_MAX,true);
    }
    auto bad=good;bad.scope=ResidualScope::AllLanes;
    rejects([&]{validate_residual(bad);},"mutated residual scope accepted");
    bad=good;++bad.proof.accepted_rewrites;
    rejects([&]{validate_residual(bad);},"mutated proof metadata accepted");
    bad=good;bad.prefix.must_root=bad.prefix.may_root;
    rejects([&]{validate_residual(bad);},"mutated care bounds accepted");
    rejects([&]{(void)compile_guarded_residual(p,12,4096,512,{262145,1000000,256});},"unbounded BDD budget accepted");
    rejects([&]{(void)compile_guarded_residual(p,12,4096,512,{65536,8000001,256});},"unbounded proof steps accepted");
    rejects([&]{(void)compile_guarded_residual(p,12,4096,512,{65536,1000000,4097});},"unbounded attempts accepted");
    const auto full=compile_guarded_residual(p,32);validate_residual(full);
    require(full.carry_roots.empty() && full.suffix.nodes[full.suffix.root].op==ResidualOp::False,"empty care did not remove residual state");
    check(p,full,values,2,UINT64_MAX,true);
    const auto folded=compile_guarded_residual(atom(10,{20,30}),12,4096,0);
    require(folded.complete && folded.prefix.loads.size()==1 && folded.prefix.loads[0].range==Bounds{20,30},"guarded proof-only certificate lost");
}
} // namespace

void test_residual() {
    exhaustive_atoms();compound_and_masks();full_width_and_random();witness_and_budgets();
    guarded_small_domains();guarded_full_width_aliases_and_ids();guarded_limits_and_witness();
}
