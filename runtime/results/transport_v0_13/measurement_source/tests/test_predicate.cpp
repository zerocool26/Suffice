#include "suffice/predicate.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using suffice::Bounds;
using suffice::i64;
constexpr auto minimum=std::numeric_limits<std::int32_t>::min();
constexpr auto maximum=std::numeric_limits<std::int32_t>::max();
void require(bool value,const char* text) {
    if(!value)throw std::runtime_error(std::string("predicate test: ")+text);
}
template<class F>void must_throw(F&& f,const char* message) {
    bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,message);
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        for(unsigned attempt=0;attempt<32;++attempt) {
            path=std::filesystem::temp_directory_path()/("suffice-predicate-"+
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+
                std::to_string(sequence.fetch_add(1)));
            if(std::filesystem::create_directory(path))return;
        }
        throw std::runtime_error("cannot create predicate test directory");
    }
    ~Directory(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
struct Fixture {
    Directory directory;
    std::vector<std::int32_t> values;
    std::uint32_t channels;
    std::unique_ptr<suffice::RawDataset> raw;
    std::unique_ptr<suffice::PackedDataset> packed;
    Fixture(std::vector<std::int32_t> input,std::uint32_t channel_count,std::uint32_t block_rows)
        :values(std::move(input)),channels(channel_count) {
        const auto raw_path=directory.path/"input.i32";
        std::ofstream output(raw_path,std::ios::binary);
        for(auto value:values) {
            const auto word=std::bit_cast<std::uint32_t>(value);
            for(unsigned b=0;b<4;++b)output.put(static_cast<char>((word>>(8*b))&255U));
        }
        output.close();if(!output)throw std::runtime_error("cannot write predicate fixture");
        const auto path=directory.path/"input.sfp";
        (void)suffice::pack_file(raw_path,path,channels,block_rows);
        raw=std::make_unique<suffice::RawDataset>(raw_path,channels);
        packed=std::make_unique<suffice::PackedDataset>(path);packed->verify_payload();
    }
    std::vector<Bounds> ranges()const {
        std::vector<Bounds> result;
        for(const auto r:packed->info().ranges)result.push_back({r.min,r.max});
        return result;
    }
};

std::vector<std::uint64_t> reference(const Fixture& fixture,const suffice::CompiledProgram& program) {
    const auto rows=fixture.packed->info().rows;
    std::vector<std::uint64_t> output(suffice::mask_words(rows));
    std::array<i64,suffice::max_program_nodes> inputs{};
    for(std::uint64_t row=0;row<rows;++row) {
        std::size_t i=0;
        for(const auto& load:program.loads())inputs[i++]=fixture.values[static_cast<std::size_t>(row)*fixture.channels+load.channel];
        if(program.evaluate_exact(std::span<const i64>(inputs.data(),i))>program.threshold())
            output[static_cast<std::size_t>(row/64)]|=std::uint64_t{1}<<(row%64);
    }
    return output;
}
void stats_check(const suffice::ExecutionStats& stats,std::span<const std::uint64_t> output,std::uint64_t rows) {
    std::uint64_t count=0;for(auto word:output)count+=std::popcount(word);
    require(stats.accepted==count,"accepted count differs from full mask");
    require(stats.rows==rows,"wrong row counter");
    require(stats.output_bytes==output.size_bytes(),"wrong output byte counter");
    require(std::accumulate(stats.resolved_by_bits.begin(),stats.resolved_by_bits.end(),std::uint64_t{0})==rows,"resolution histogram differs from rows");
    require(stats.refined_rows<=rows,"refinement count exceeds rows");
    if(rows%64)require((output.back()>>(rows%64))==0,"nonzero output padding");
}
void check(Fixture& fixture,const suffice::CompiledProgram& source) {
    const auto plan=suffice::compile_predicate(source);
    if(!plan.complete)throw std::runtime_error("unexpected predicate fallback: "+plan.reason+"; "+source.describe());
    const auto expected=reference(fixture,source);
    std::vector<std::uint64_t> output(expected.size(),~std::uint64_t{0});
    suffice::EncodedPredicateExecutor encoded(*fixture.packed,plan);
    suffice::DensePredicateExecutor dense(*fixture.raw,plan,true),scalar(*fixture.raw,plan,false);
    require(encoded.scratch_bytes()<=encoded.required_scratch_bytes(plan),"encoded scratch exceeds preparation estimate");
    require(dense.scratch_bytes()<=dense.required_scratch_bytes(plan),"dense scratch exceeds preparation estimate");
    auto run=[&](auto& executor) {
        auto stats=executor.run(output);
        require(output==expected,"lowered mask differs from original exact IR");
        stats_check(stats,output,fixture.packed->info().rows);
        std::fill(output.begin(),output.end(),~std::uint64_t{0});
        allocation_probe::start();
        try {stats=executor.run(output);}catch(...){allocation_probe::stop();throw;}
        const auto allocations=allocation_probe::stop();
        require(allocations==0,"prepared Boolean execution allocated heap memory");
        require(output==expected,"repeated prepared run differs");
        stats_check(stats,output,fixture.packed->info().rows);
        return stats;
    };
    const auto stats=run(encoded);run(dense);run(scalar);
    require(stats.requested_payload_bytes<=fixture.packed->info().file_bytes-fixture.packed->info().metadata_bytes,"encoded executor requested more than the entire payload");
    require(stats.loaded_planes<=32*fixture.packed->block_count()*plan.load_count,"plane views were not cached per block");
}

void exhaustive_small() {
    std::vector<std::int32_t> values;
    for(int x=-8;x<=8;++x)for(int y=-8;y<=8;++y) {
        values.push_back(x);values.push_back(y);values.push_back((x*3+y*5)%9);
    }
    for(auto block:{32U,64U,4096U}) {
        Fixture fixture(values,3,block);
        for(i64 threshold=-12;threshold<=12;++threshold)for(unsigned kind=0;kind<8;++kind) {
            suffice::Program p;const auto x=p.load(0),y=p.load(1),z=p.load(2);auto score=x;
            switch(kind) {
            case 0:score=p.min(x,y);break;
            case 1:score=p.max(p.abs(x),p.abs(y));break;
            case 2:score=p.min(p.abs(x),p.abs(y));break;
            case 3:score=p.abs(p.min(x,p.mul_const(y,-3)));break;
            case 4:score=p.square(p.max(x,y));break;
            case 5:score=p.max(p.min(p.abs(p.sub(p.mul_const(x,3),p.constant(7))),p.abs(p.add(y,p.constant(2)))),p.square(z));break;
            case 6:score=p.sub(p.constant(7),p.min(x,y));break;
            default:score=p.mul_const(p.min(x,y),-7);break;
            }
            p.output(score,threshold);check(fixture,p.compile(fixture.ranges()));
        }
    }
}
void tails_and_extremes() {
    for(auto rows:{0U,1U,2U,7U,8U,31U,32U,33U,63U,64U,65U,127U,129U,4097U}) {
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows)*3);
        const std::array<std::int32_t,9> fixed{{minimum,maximum,-100001,-1,0,1,100000,100001,23}};
        for(std::size_t i=0;i<values.size();++i)values[i]=fixed[(i*5+i/9)%fixed.size()];
        for(auto block:{32U,64U,4096U}) {
            Fixture fixture(values,3,block);
            for(const auto threshold:std::array<i64,9>{{std::numeric_limits<i64>::min(),-1,0,1,100000,2147483647LL,2147483648LL,4611686018427387903LL,std::numeric_limits<i64>::max()}}) {
                suffice::Program p;const auto x=p.load(0),y=p.load(2);
                p.output(p.max(p.square(x),p.abs(p.mul_const(y,3))),threshold);
                check(fixture,p.compile(fixture.ranges()));
            }
        }
    }
}
void inverse_arithmetic_boundaries() {
    for(auto coefficient:{-7,-3,-1,0,1,3,7}) {
        std::vector<std::int32_t> values;for(int x=-8;x<=8;++x)values.push_back(x);
        Fixture fixture(std::move(values),1,32);
        for(i64 t=-60;t<=60;++t) {
            suffice::Program p;p.output(p.mul_const(p.load(0),coefficient),t);
            check(fixture,p.compile(fixture.ranges()));
        }
    }
    Fixture fixture({0,1,0,1,1,0},1,32);
    const auto hi=std::numeric_limits<i64>::max(),lo=std::numeric_limits<i64>::min();
    for(unsigned kind=0;kind<5;++kind)for(auto t:std::array<i64,7>{{lo,lo+1,-1,0,1,hi-1,hi}}) {
        suffice::Program p;const auto x=p.load(0);auto score=x;
        if(kind==0)score=p.add(p.constant(hi-1),x);
        if(kind==1)score=p.add(p.constant(lo),x);
        if(kind==2)score=p.mul_const(x,lo);
        if(kind==3)score=p.sub(p.constant(hi),x);
        if(kind==4)score=p.mul_const(x,hi);
        p.output(score,t);check(fixture,p.compile(fixture.ranges()));
    }
}
void budgets_and_contracts() {
    const std::array<Bounds,2> ranges{{{-100,100},{-100,100}}};
    suffice::Program p;const auto x=p.load(0),y=p.load(1);p.output(p.min(p.abs(x),p.abs(y)),1);
    const auto source=p.compile(ranges);
    for(auto budget:{0U,1U,2U,257U}) {
        const auto plan=suffice::compile_predicate(source,budget);
        require(!plan.complete&&plan.node_count==0&&plan.load_count==0&&plan.certificate_count==0,
                "partial or invalid-budget graph escaped failure");
        for(const auto& node:plan.nodes)require(node==suffice::PredicateNode{},"failed graph was not cleared");
    }
    for(bool subtract:{false,true}) {
        suffice::Program q;const auto a=q.load(0),b=q.load(1);q.output(subtract?q.sub(a,b):q.add(a,b),0);
        const auto rejected=suffice::compile_predicate(q.compile(ranges));
        require(!rejected.complete&&!rejected.node_count,"unsupported two-input arithmetic accepted");
    }
    suffice::Program shifted;shifted.output(shifted.load(0,1),0);
    require(!suffice::compile_predicate(shifted.compile(ranges)).complete,"shifted predicate accepted");
    require(!suffice::compile_predicate(suffice::CompiledProgram{}).complete,"uncompiled source accepted");
    Fixture fixture({minimum,maximum,0},1,32);
    const auto invalid=suffice::compile_predicate(suffice::CompiledProgram{});
    must_throw([&]{suffice::EncodedPredicateExecutor e(*fixture.packed,invalid);},"incomplete packed plan accepted");
    must_throw([&]{suffice::DensePredicateExecutor e(*fixture.raw,invalid);},"incomplete dense plan accepted");
    suffice::Program narrow;narrow.output(narrow.load(0),0);
    const auto plan=suffice::compile_predicate(narrow.compile(std::span<const Bounds>(ranges.data(),1)));
    must_throw([&]{suffice::EncodedPredicateExecutor e(*fixture.packed,plan);},"narrow packed range accepted");
    must_throw([&]{suffice::DensePredicateExecutor e(*fixture.raw,plan);},"narrow raw range accepted");
    auto bad=plan;bad.nodes[bad.root].op=suffice::PredicateOp::Not;bad.nodes[bad.root].a=bad.root;
    must_throw([&]{suffice::EncodedPredicateExecutor e(*fixture.packed,bad);},"cyclic Boolean plan accepted");
}
void retained_range_certificates() {
    const std::array<Bounds,1> narrow_range{{{0,10}}};
    Fixture narrow({0,3,10},1,32),above({0,101,10},1,32),below({0,-1,10},1,32);
    auto reject_both=[&](Fixture& fixture,const suffice::PredicatePlan& plan) {
        must_throw([&]{suffice::EncodedPredicateExecutor e(*fixture.packed,plan);},
                   "packed executor discarded a folded range certificate");
        must_throw([&]{suffice::DensePredicateExecutor e(*fixture.raw,plan);},
                   "dense executor discarded a folded range certificate");
    };
    for(const auto threshold:{100,-1}) {
        suffice::Program p;p.output(p.load(0),threshold);
        const auto source=p.compile(narrow_range);
        const auto plan=suffice::compile_predicate(source);
        require(plan.complete&&plan.load_count==0&&plan.certificate_count==1,
                "constant predicate did not retain its source certificate");
        require(plan.certificates[0].channel==0&&plan.certificates[0].range==narrow_range[0],
                "constant predicate changed its source certificate");
        reject_both(above,plan);reject_both(below,plan);
        check(narrow,source);
        std::vector<std::uint64_t> output(suffice::mask_words(narrow.raw->info().rows));
        suffice::EncodedPredicateExecutor encoded(*narrow.packed,plan);
        suffice::DensePredicateExecutor dense(*narrow.raw,plan,true,true);
        const auto packed_stats=encoded.run(output),dense_stats=dense.run(output);
        require(output==reference(narrow,source),"trusted constant predicate output differs");
        require(packed_stats.loaded_planes==0&&packed_stats.requested_payload_bytes==0&&
                dense_stats.requested_payload_bytes==0&&dense_stats.exact_evaluations==0,
                "proof-only certificates caused constant execution input reads");
    }

    // x is eliminated by range folding while y remains an executable leaf.
    // A broader x would change the original answer on the first row.
    const std::array<Bounds,2> branch_ranges{{{0,10},{-200,200}}};
    Fixture branch_ok({0,-200,10,200,3,0,7,101},2,32);
    Fixture branch_bad({101,-200,10,200,3,0,7,101},2,32);
    suffice::Program p;const auto x=p.load(0),y=p.load(1);p.output(p.max(x,y),100);
    const auto source=p.compile(branch_ranges);
    const auto branch=suffice::compile_predicate(source);
    require(branch.complete&&branch.load_count==1&&branch.loads[0].channel==1&&
            branch.certificate_count==2,"pruned branch did not retain all source certificates");
    reject_both(branch_bad,branch);check(branch_ok,source);
    std::vector<std::uint64_t> output(suffice::mask_words(branch_ok.raw->info().rows));
    suffice::DensePredicateExecutor dense(*branch_ok.raw,branch,false);
    const auto stats=dense.run(output);
    require(stats.requested_payload_bytes==branch_ok.raw->info().rows*4,
            "proof-only branch certificate became an execution load");

    // Hand-authored plans can omit certificates; their live leaves still impose
    // contracts. Trusted construction skips data scans, not structural checks.
    suffice::Program live;live.output(live.load(0),5);
    auto manual=suffice::compile_predicate(live.compile(narrow_range));
    manual.certificates={};manual.certificate_count=0;
    reject_both(above,manual);
    suffice::EncodedPredicateExecutor manual_encoded(*narrow.packed,manual);
    suffice::DensePredicateExecutor manual_dense(*narrow.raw,manual);
    manual_encoded.run(output);manual_dense.run(output);

    suffice::Program folded;folded.output(folded.load(0),100);
    const auto constant=suffice::compile_predicate(folded.compile(narrow_range));
    auto reject_structure=[&](const suffice::PredicatePlan& invalid) {
        reject_both(narrow,invalid);
        must_throw([&]{suffice::DensePredicateExecutor e(*narrow.raw,invalid,true,true);},
                   "trusted constructor bypassed certificate structure validation");
    };
    auto bad=constant;bad.certificate_count=suffice::max_program_nodes+1;reject_structure(bad);
    bad=constant;bad.certificates[0].range={10,0};reject_structure(bad);
    bad=constant;bad.certificates[0].range={i64{minimum}-1,10};reject_structure(bad);
    bad=constant;bad.certificates[0].range={0,i64{maximum}+1};reject_structure(bad);
    bad=constant;bad.certificates[0].offset=suffice::max_input_offset+1;reject_structure(bad);
    bad=constant;bad.certificates[0].channel=1;reject_structure(bad);
    for(const auto offset:{-suffice::max_input_offset,suffice::max_input_offset}) {
        auto shifted=constant;shifted.certificates[0].offset=offset;
        suffice::EncodedPredicateExecutor encoded(*narrow.packed,shifted);
        suffice::DensePredicateExecutor raw(*narrow.raw,shifted);
        encoded.run(output);raw.run(output);
        reject_both(above,shifted);
    }
}
void short_circuit_and_cache() {
    std::vector<std::int32_t> values;
    // Threshold zero keeps this on the active-lane path instead of the eager
    // sign-DAG batch specialization. A +2 requires exactly 31 leading planes;
    // a negative input is rejected after the first plane.
    for(unsigned row=0;row<128;++row){values.push_back(row<64?-2:2);values.push_back(row%2?-2:2);}
    Fixture fixture(std::move(values),2,4096);
    for(bool use_or:{false,true}) {
        suffice::Program p;const auto x=p.load(0),y=p.load(1);p.output(use_or?p.max(x,y):p.min(x,y),0);
        const auto source=p.compile(fixture.ranges());const auto plan=suffice::compile_predicate(source);
        suffice::EncodedPredicateExecutor executor(*fixture.packed,plan);
        std::vector<std::uint64_t> output(suffice::mask_words(128));const auto stats=executor.run(output);
        require(output==reference(fixture,source),"short-circuit output mismatch");
        require(stats.kernel=="encoded-predicate","non-sign predicate incorrectly chose sign batches");
        require(stats.requested_payload_bytes==504,"inactive branch still fetched its plane words");
    }
    Fixture extremes({minimum,maximum,-1,0,1,-1000,1000},1,32);
    suffice::Program p;const auto x=p.load(0);p.output(p.abs(x),1);
    const auto source=p.compile(extremes.ranges()),plan_source=source;
    const auto plan=suffice::compile_predicate(plan_source);
    suffice::EncodedPredicateExecutor executor(*extremes.packed,plan);
    std::vector<std::uint64_t> output(suffice::mask_words(7));const auto stats=executor.run(output);
    require(stats.cache_reuses>0,"two thresholds did not share encoded plane words");
    require(output==reference(extremes,source),"shared plane cache changed answer");
}
void sign_operator_batches() {
    for(auto rows:{0U,1U,7U,31U,32U,33U,63U,64U,65U,4097U,16385U,32769U}) {
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows)*3);
        const std::array<std::int32_t,7> fixed{{minimum,maximum,-1,0,1,-17,23}};
        for(std::size_t i=0;i<values.size();++i)values[i]=fixed[(i*3+i/11)%fixed.size()];
        for(auto block:{32U,64U,4096U,65536U}) {
            Fixture fixture(values,3,block);
            for(unsigned kind=0;kind<8;++kind) {
                suffice::Program p;const auto x=p.load(0),y=p.load(1),z=p.load(2);
                auto negate=[&](suffice::NodeId a){return p.sub(p.mul_const(a,-1),p.constant(1));};
                auto score=x;
                switch(kind) {
                case 0:score=p.min(x,y);break;
                case 1:score=p.max(x,y);break;
                case 2:score=p.min(x,negate(y));break;
                case 3:score=negate(p.max(p.min(x,y),p.min(negate(x),z)));break;
                case 4:score=p.max(p.min(x,y),p.min(x,z));break;
                case 5:score=p.max(p.min(x,y),negate(p.min(y,z)));break;
                case 6:score=p.max(x,negate(x));break;
                default:score=p.min(x,negate(x));break;
                }
                p.output(score,-1);const auto source=p.compile(fixture.ranges());
                check(fixture,source);
                const auto plan=suffice::compile_predicate(source);
                suffice::EncodedPredicateExecutor executor(*fixture.packed,plan);
                std::vector<std::uint64_t> output(suffice::mask_words(rows));
                const auto stats=executor.run(output);
                if constexpr(std::endian::native==std::endian::little) {
                    require(stats.kernel=="encoded-sign-dag","sign graph did not choose batched operator route");
                    const auto expected_bytes=static_cast<std::uint64_t>(plan.load_count)*
                        (static_cast<std::uint64_t>(rows/block)*(block/8)+(rows%block+7)/8);
                    require(stats.requested_payload_bytes==expected_bytes,"sign route logical plane accounting differs");
                    require(stats.resolved_by_bits[plan.load_count?1:0]==rows,"sign route depth accounting differs");
                    if(rows>1&&kind==5)require(executor.scratch_bytes()<plan.node_count*2048,"sign operator arena did not reuse live slots");
                }
                require(output==reference(fixture,source),"batched sign DAG differs from original IR");
            }
        }
    }
}
void randomized_compositions() {
    std::vector<std::int32_t> values;for(int i=0;i<65*3;++i)values.push_back((i*17+i/5)%17-8);
    Fixture fixture(std::move(values),3,64);
    std::uint32_t state=0x91ac7e3dU;
    auto random=[&](){state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    unsigned checked=0;
    for(unsigned attempt=0;attempt<300;++attempt) {
        suffice::Program p;std::vector<suffice::NodeId> nodes{p.load(0),p.load(1),p.load(2)};
        for(unsigned k=0;k<10;++k) {
            const auto a=nodes[random()%nodes.size()],b=nodes[random()%nodes.size()];
            const i64 c=static_cast<i64>(random()%11)-5;auto n=a;
            switch(random()%8) {
            case 0:n=p.min(a,b);break;case 1:n=p.max(a,b);break;
            case 2:n=p.abs(a);break;case 3:n=p.square(a);break;
            case 4:n=p.add(a,p.constant(c));break;case 5:n=p.sub(p.constant(c),a);break;
            case 6:n=p.mul_const(a,c);break;default:n=p.sub(a,p.constant(c));break;
            }
            nodes.push_back(n);
        }
        p.output(nodes.back(),static_cast<i64>(random()%201)-100);
        try {const auto source=p.compile(fixture.ranges());check(fixture,source);++checked;}
        catch(const std::overflow_error&) { /* The original compiler rejects unsafe integer graphs. */ }
    }
    require(checked>=250,"too few safe random compositions were checked");
}
} // namespace

void test_predicate() {
    exhaustive_small();tails_and_extremes();inverse_arithmetic_boundaries();
    budgets_and_contracts();retained_range_certificates();short_circuit_and_cache();randomized_compositions();
    sign_operator_batches();
}
