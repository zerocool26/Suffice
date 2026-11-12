#include "suffice/portfolio.hpp"
#include "suffice/portfolio_codegen.hpp"
#include "suffice/benchmark_order.hpp"
#include "allocation_probe.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace suffice;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("portfolio test: ") + message);
}
template<class Exception = std::invalid_argument, class F>
void rejects(F&& callback, const char* message) {
    bool rejected = false;
    try { callback(); } catch (const Exception&) { rejected = true; }
    require(rejected, message);
}

PortfolioCalibrationOptions options() {
    PortfolioCalibrationOptions result;
    result.max_row_evaluations = 1000000;
    result.max_elapsed_nanoseconds = std::numeric_limits<std::uint64_t>::max();
    result.expected_remaining_calls = 1000000000;
    return result;
}

std::array<PortfolioCycleObservation, 3> observations(
    std::array<double,4> a, std::array<double,4> b, std::array<double,4> c) {
    std::array<PortfolioCycleObservation, 3> result{};
    const std::array inputs{a,b,c};
    for (unsigned cycle = 0; cycle < inputs.size(); ++cycle)
        std::copy(inputs[cycle].begin(), inputs[cycle].end(), result[cycle].median_nanoseconds.begin());
    return result;
}

void decision_rule() {
    auto limits = options();
    limits.expected_remaining_calls = 5;
    auto times = observations({1000,900,800,1200},{1000,900,800,1200},{1000,900,800,1200});
    auto decision = decide_portfolio(times,4,0,1000,limits);
    require(decision.changed && decision.selected_index==2 && decision.proposed_index==2,
            "consistent fastest admissible candidate not selected");
    require(decision.estimated_break_even_calls==5 && decision.estimated_saving_nanoseconds==200,
            "break-even arithmetic differs from synthetic costs");
    limits.expected_remaining_calls=4;
    decision=decide_portfolio(times,4,0,1000,limits);
    require(!decision.changed && decision.selected_index==0 && decision.proposed_index==2 &&
            decision.reason==PortfolioReason::NotAmortized,"reuse gate did not retain incumbent");
    limits.expected_remaining_calls=100;
    times=observations({1000,800,800,1200},{1000,800,800,1200},{1000,800,800,1200});
    require(decide_portfolio(times,4,0,1000,limits).selected_index==1,"tie was not resolved by lowest candidate index");
    // The very good first two cycles do not excuse a bad third cycle.
    times=observations({1000,500,1200,1200},{1000,500,1200,1200},{1000,990,1200,1200});
    require(decide_portfolio(std::span(times).first(2),4,0,1000,limits).selected_index==1,
            "two complete consistent cycles rejected");
    require(decide_portfolio(times,4,0,1000,limits).reason==PortfolioReason::NoConsistentImprovement,
            "median aggregation hid a failed per-cycle margin");
    times=observations({1000,980,1200,1200},{1000,980,1200,1200},{1000,980,1200,1200});
    require(decide_portfolio(times,4,0,1000,limits).selected_index==1,"exact improvement margin boundary rejected");
    times=observations({1025,1004.5,1200,1200},{1025,1004.5,1200,1200},{1025,1004.5,1200,1200});
    require(decide_portfolio(times,4,0,1000,limits).selected_index==1,"half-nanosecond exact margin boundary rejected");
    rejects([]{(void)portfolio_detail::median(std::array<double,8>{},0);},"empty bounded median accepted");
    rejects([]{(void)portfolio_detail::median(std::array<double,8>{},9);},"oversized bounded median accepted");
    require(decide_portfolio(std::span(times).first(1),4,0,1000,limits).reason==PortfolioReason::InsufficientCycles,
            "one cycle was enough to select");
    times=observations({1000,900,800,700},{1000,900,800,700},{1000,900,800,700});
    require(decide_portfolio(times,4,2,1000,limits).selected_index==3,"nonzero incumbent ignored");
    limits.expected_remaining_calls=0;
    require(decide_portfolio(times,4,2,1000,limits).reason==PortfolioReason::NoExpectedReuse,"implicit reuse assumption accepted");
    limits=options();
    times=observations({1000,0,1200,1200},{1000,0,1200,1200},{1000,0,1200,1200});
    require(!decide_portfolio(times,4,0,1000,limits).changed,"zero-resolution timing won");
    times=observations({0,0,1200,1200},{0,0,1200,1200},{0,0,1200,1200});
    require(!decide_portfolio(times,4,0,1000,limits).changed,"zero baseline caused invalid saving");
    limits.minimum_improvement_basis_points=0;
    times=observations({1000,999,1200,1200},{1000,999,1200,1200},{1000,999,1200,1200});
    limits.expected_remaining_calls=std::numeric_limits<std::uint64_t>::max();
    decision=decide_portfolio(times,4,0,std::numeric_limits<std::uint64_t>::max(),limits);
    require(decision.break_even_saturated && !decision.changed,"break-even overflow was not conservative");
    rejects([&]{(void)decide_portfolio(times,0,0,1,limits);},"zero candidate decision accepted");
    rejects([&]{(void)decide_portfolio(times,9,0,1,limits);},"too many candidates accepted");
    rejects([&]{(void)decide_portfolio(times,4,4,1,limits);},"invalid incumbent accepted");
    times[0].median_nanoseconds[0]=std::numeric_limits<double>::quiet_NaN();
    rejects([&]{(void)decide_portfolio(times,4,0,1,limits);},"NaN timing accepted");
    times[0].median_nanoseconds[0]=std::numeric_limits<double>::infinity();
    rejects([&]{(void)decide_portfolio(times,4,0,1,limits);},"infinite timing accepted");
    times[0].median_nanoseconds[0]=-1;
    rejects([&]{(void)decide_portfolio(times,4,0,1,limits);},"negative timing accepted");
    // Runtime samples are uint64 nanoseconds. Preserve their rounded double
    // endpoint while rejecting values that could overflow the scaled margin
    // products on targets whose long double has only double's range.
    limits=options();
    const auto maximum_observation=static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    times=observations({maximum_observation,maximum_observation*0.99,maximum_observation,maximum_observation},
                       {maximum_observation,maximum_observation*0.99,maximum_observation,maximum_observation},
                       {maximum_observation,maximum_observation*0.99,maximum_observation,maximum_observation});
    require(decide_portfolio(times,4,0,1,limits).reason==PortfolioReason::NoConsistentImprovement,
            "rounded timer endpoint was rejected or a sub-margin change was admitted");
    for(auto& cycle:times)cycle.median_nanoseconds[1]=maximum_observation*0.9;
    require(decide_portfolio(times,4,0,1,limits).selected_index==1,
            "valid timer-domain endpoint failed a clear improvement");
    times[1].median_nanoseconds[3]=std::nextafter(maximum_observation,std::numeric_limits<double>::infinity());
    rejects([&]{(void)decide_portfolio(times,4,0,1,limits);},"timing above rounded uint64 domain accepted");
    times=observations({1e308,9.9e307,1e308,1e308},{1e308,9.9e307,1e308,1e308},{1e308,9.9e307,1e308,1e308});
    rejects([&]{(void)decide_portfolio(times,4,0,1,limits);},"finite overflow-domain diagnostic timings accepted");
}

void balanced_orders() {
    for (unsigned count=2;count<=portfolio_max_candidates;++count) {
        const auto rounds=portfolio_detail::cycle_rows(count);
        std::uint64_t seed=0x1234567;
        const auto dynamic=balanced_timing_orders(count,3*rounds,seed);
        for(unsigned cycle=0;cycle<3;++cycle) {
            const auto order=portfolio_detail::timing_cycle(count,seed);
            std::array<std::array<unsigned,portfolio_max_candidates>,portfolio_max_candidates> position{}, predecessor{};
            for(unsigned row=0;row<rounds;++row) {
                std::array<bool,portfolio_max_candidates> seen{};
                for(unsigned column=0;column<count;++column) {
                    const auto label=order.orders[row][column];
                    require(label<count&&!seen[label],"timing row is not a candidate permutation");
                    seen[label]=true;++position[label][column];
                    if(column)++predecessor[order.orders[row][column-1]][label];
                    require(label==dynamic[cycle*rounds+row][column],"fixed order differs from established Williams generator");
                }
            }
            for(unsigned a=0;a<count;++a)for(unsigned b=0;b<count;++b) {
                require(position[a][b]==rounds/count,"positions are not balanced");
                require(predecessor[a][b]==(a==b?0:rounds/count),"within-round predecessors are not balanced");
            }
        }
    }
}

struct FakeKernel {
    bool wrong_mask{}, wrong_accepted{}, throws{}, omit_last_word{};
    std::uint64_t calls{};
    static std::uint64_t run(void* context,std::uint64_t first,std::uint64_t count,
                             std::span<std::uint64_t> mask) {
        auto& self=*static_cast<FakeKernel*>(context);++self.calls;
        if(self.throws)throw std::runtime_error("intentional callback failure");
        std::uint64_t accepted=0;
        for(std::size_t i=0;i<mask.size();++i) {
            std::uint64_t word=0;
            for(unsigned bit=0;bit<64&&i*64+bit<count;++bit)
                if((first+i*64+bit)%3==0)word|=std::uint64_t{1}<<bit;
            accepted+=std::popcount(word);
            if(!(self.omit_last_word&&i+1==mask.size()))mask[i]=word;
        }
        if(self.wrong_mask&&!mask.empty())mask[0]^=1;
        return accepted+self.wrong_accepted;
    }
};

void runtime_contracts() {
    std::array<FakeKernel,4> kernels{};
    std::array<PortfolioCandidate,4> candidates{{
        {"a",&kernels[0],&FakeKernel::run,false},{"b",&kernels[1],&FakeKernel::run,false},
        {"c",&kernels[2],&FakeKernel::run,true},{"d",&kernels[3],&FakeKernel::run,false}
    }};
    BoundedPortfolio portfolio(65,candidates,2);
    require(portfolio.selected_index()==2&&portfolio.selected_name()=="c"&&portfolio.uses_simd(),
            "initial incumbent metadata invalid");
    std::array<std::uint64_t,2> expected{},scratch{};
    const auto wanted=FakeKernel::run(&kernels[0],0,65,expected);
    auto limits=options();limits.max_row_evaluations=52*65;
    allocation_probe::start();
    const auto report=portfolio.calibrate(0,65,expected,scratch,limits);
    const auto accepted=portfolio.run(scratch);
    const auto allocations=allocation_probe::stop();
    require(allocations==0,"valid prepared calibration or execution allocated");
    require(accepted==wanted&&scratch==expected,"selected execution differs from exact mask");
    require(report.completed_cycles==3&&report.total_calls==52&&report.row_evaluations==52*65,
            "complete cycle or warmup work accounting mismatch");
    require(report.stop_reason==PortfolioStopReason::Completed,"unexpected cycle stop reason");
    for(unsigned index=0;index<4;++index) {
        const auto& m=report.candidates[index];
        require(m.calls==13&&m.warmup_calls==1&&m.timed_samples==12&&m.row_evaluations==13*65,
                "per-candidate calls do not include warmups");
        for(unsigned cycle=0;cycle<3;++cycle)
            require(m.cycle_samples[cycle]==4&&m.cycle_median_nanoseconds[cycle]==report.cycles[cycle].median_nanoseconds[index],
                    "per-cycle samples or median metadata mismatch");
    }
    // Recalibration must use entirely fresh observations. A workless call has
    // zero samples even when the previous calibration may have changed winner.
    const auto selected=portfolio.selected_index();
    limits.expected_remaining_calls=0;
    const auto skipped=portfolio.calibrate(0,65,expected,scratch,limits);
    require(skipped.total_calls==0&&skipped.completed_cycles==0&&skipped.selected_index==selected,
            "disabled recalibration reused stale observations");
    for(const auto& m:skipped.candidates)require(m.calls==0&&m.timed_samples==0,"stale candidate stats survived");
    limits=options();limits.max_row_evaluations=36*65;
    const auto bounded=portfolio.calibrate(0,65,expected,scratch,limits);
    require(bounded.completed_cycles==2&&bounded.total_calls==36&&bounded.stop_reason==PortfolioStopReason::RowBudget,
            "row budget did not stop at complete balanced cycles");
    limits.max_row_evaluations=36*65-1;
    const auto too_small=portfolio.calibrate(0,65,expected,scratch,limits);
    require(too_small.total_calls==0&&too_small.reason==PortfolioReason::InsufficientRowBudget,
            "budget below minimum cycles spent work");
    limits=options();limits.max_elapsed_nanoseconds=0;
    require(portfolio.calibrate(0,65,expected,scratch,limits).total_calls==0,"disabled time budget spent calls");
    limits=options();
    const auto before_failure=portfolio.selected_index();
    for(unsigned kind=0;kind<4;++kind) {
        kernels[1].wrong_mask=kind==0;kernels[1].wrong_accepted=kind==1;
        kernels[1].throws=kind==2;kernels[1].omit_last_word=kind==3;
        rejects<std::runtime_error>([&]{(void)portfolio.calibrate(0,65,expected,scratch,limits);},
                                    "mismatched or failing candidate accepted");
        require(portfolio.selected_index()==before_failure,"failed recalibration changed incumbent");
    }
    kernels[1]={};
    rejects([&]{(void)portfolio.candidate(4);},"invalid candidate index accepted");
    rejects([&]{(void)portfolio.run_range(66,0,{});},"range past dataset accepted");
    rejects([&]{(void)portfolio.run_range(1,std::numeric_limits<std::uint64_t>::max(),scratch);},"overflow range accepted");
    rejects([&]{(void)portfolio.run(std::span(scratch).first(1));},"short output accepted");
    require(portfolio.run_range(65,0,{})==0,"empty end range rejected");
    require(portfolio.calibrate(65,0,{},{},limits).reason==PortfolioReason::EmptyRange,"empty calibration was measured");
    rejects([&]{(void)portfolio.calibrate(0,65,std::span(expected).first(1),scratch,limits);},"short oracle accepted");
    rejects([&]{(void)portfolio.calibrate(0,65,expected,expected,limits);},"identical expected and scratch accepted");
    std::array<std::uint64_t,3> overlapping{};
    rejects([&]{(void)portfolio.calibrate(0,65,std::span(overlapping).first(2),std::span(overlapping).last(2),limits);},
            "forward partial alias accepted");
    rejects([&]{(void)portfolio.calibrate(0,65,std::span(overlapping).last(2),std::span(overlapping).first(2),limits);},
            "reverse partial alias accepted");
    const auto saved=expected.back();expected.back()|=2;
    rejects([&]{(void)portfolio.calibrate(0,65,expected,scratch,limits);},"nonzero oracle padding accepted");
    expected.back()=saved;
    for(unsigned kind=0;kind<6;++kind) {
        auto invalid=limits;
        if(kind==0)invalid.cycles=1;
        if(kind==1)invalid.cycles=portfolio_max_cycles+1;
        if(kind==2)invalid.minimum_cycles=1;
        if(kind==3)invalid.minimum_cycles=invalid.cycles+1;
        if(kind==4)invalid.minimum_improvement_basis_points=10000;
        if(kind==5)invalid.warmups_per_candidate=5;
        rejects([&]{(void)portfolio.calibrate(0,65,expected,scratch,invalid);},"invalid calibration limits accepted");
    }
    require(portfolio.selected_index()==before_failure,"invalid validation paths changed selection");
    rejects([&]{BoundedPortfolio invalid(65,{});},"empty portfolio accepted");
    rejects([&]{BoundedPortfolio invalid(65,candidates,4);},"invalid initial index accepted");
    auto bad=candidates;bad[1].run_range=nullptr;
    rejects([&]{BoundedPortfolio invalid(65,bad);},"null callback accepted");
    bad=candidates;bad[1].context=nullptr;
    rejects([&]{BoundedPortfolio invalid(65,bad);},"null context accepted");
    bad=candidates;bad[1].name="a";
    rejects([&]{BoundedPortfolio invalid(65,bad);},"duplicate candidate identity accepted");
    BoundedPortfolio huge(std::numeric_limits<std::uint64_t>::max(),candidates);
    require(huge.run_range(std::numeric_limits<std::uint64_t>::max(),0,{})==0,"max-row empty range overflowed");
    rejects([&]{(void)huge.run_range(std::numeric_limits<std::uint64_t>::max(),1,std::span(scratch).first(1));},
            "max-row nonempty range overflow accepted");
    BoundedPortfolio one(65,std::span(candidates).first(1));
    require(one.calibrate(0,65,expected,scratch,limits).reason==PortfolioReason::SingleCandidate,"single-candidate calibration spent work");
}

void emission_contracts() {
    const std::array<Bounds,2> ranges{{{-2147483648LL,2147483647LL},{-2147483648LL,2147483647LL}}};
    const auto program=parse_program("input x 0 0\ninput y 1 0\nmin both x y\noutput both > 1000\n").compile(ranges);
    CodegenOptions base;base.namespace_name="portfolio_test";
    const auto generated=emit_cpp_portfolio(program,base);
    require(generated.variants.size()==4,"portfolio is not fixed at four candidates");
    require(generated.header==emit_cpp_portfolio(program,base).header,"portfolio emission is not deterministic");
    require(generated.header.find("class PreparedEncodedPortfolio")!=std::string::npos&&
            generated.header.find("#include \"suffice/portfolio.hpp\"")!=std::string::npos,
            "owning generated runtime missing");
    for(unsigned index=0;index<4;++index)
        require(generated.header.find("mutable K"+std::to_string(index)+" k"+std::to_string(index)+"_;")!=std::string::npos,
                "const portfolio execution would mutate a non-mutable child cache");
    for(unsigned index=0;index<4;++index) {
        const auto& variant=generated.variants[index];
        require(variant.namespace_name==base.namespace_name+"PortfolioV"+std::to_string(index),"variant namespace collision");
        require(variant.kernel.encoded_residual_emitted&&variant.kernel.encoded_emitted,"portfolio variant not executable");
        require(variant.kernel.encoded_guarded_residual_emitted==bool(index%2),"variant care-mask strategy mismatch");
        require(variant.kernel.encoded_recompute_carry_emitted==bool(index>=2),"variant carry policy mismatch");
    }
    base.enable_avx2=false;
    const auto scalar=emit_cpp_portfolio(program,base);
    for(const auto& variant:scalar.variants)require(!variant.kernel.encoded_avx2_emitted,"scalar-only portfolio ignored");
    for(const auto* name:{"class","std","_bad","bad__name","a::b","bad;code",""}) {
        base.namespace_name=name;
        rejects([&]{(void)emit_cpp_portfolio(program,base);},"invalid outer portfolio namespace accepted");
    }
    base.namespace_name="trailing_";
    require(!emit_cpp_portfolio(program,base).header.empty(),"valid trailing underscore namespace rejected");
    base.enable_encoded=false;
    rejects([&]{(void)emit_cpp_portfolio(program,base);},"disabled encoded portfolio accepted");
    base.enable_encoded=true;base.lower_predicates=false;
    rejects([&]{(void)emit_cpp_portfolio(program,base);},"unsupported encoded lowering accepted");
    base.lower_predicates=true;
    rejects([&]{(void)emit_cpp_portfolio(program,base,{.max_header_bytes=0});},"zero output budget accepted");
    rejects<std::length_error>([&]{(void)emit_cpp_portfolio(program,base,{.max_header_bytes=64});},"combined header exceeded bounded bytes");
}
} // namespace

void test_portfolio() {
    decision_rule();balanced_orders();runtime_contracts();emission_contracts();
}
