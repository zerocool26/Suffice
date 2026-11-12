#include "suffice/runtime.hpp"
#include "suffice/vector.hpp"
#include "suffice/predicate.hpp"
#include "suffice/encoded_ranges.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using suffice::Bounds;
using suffice::i64;
constexpr std::uint64_t guard = 0xa19ce8b7d32546f0ULL;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("row-range test: ") + message);
}

template<class Function> void rejects(Function action) {
    bool threw=false;
    try { action(); } catch (const std::invalid_argument&) { threw=true; }
    check(threw,"invalid range or mask was accepted");
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::RawDataset> raw;
    std::unique_ptr<suffice::PackedDataset> packed;
    std::uint64_t rows;
    std::uint32_t channels;
    Fixture(std::uint64_t count,std::uint32_t width,std::uint32_t block):rows(count),channels(width) {
        static std::atomic<unsigned> counter{0};
        directory=std::filesystem::temp_directory_path()/
            ("suffice-range-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
             "-"+std::to_string(counter.fetch_add(1)));
        if(!std::filesystem::create_directory(directory))throw std::runtime_error("range test temporary directory");
        values.resize(static_cast<std::size_t>(rows)*channels);
        std::uint32_t random=0x82ba19c3u;
        for(std::size_t i=0;i<values.size();++i) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            switch(i%11) {
            case 0:values[i]=std::numeric_limits<std::int32_t>::min();break;
            case 1:values[i]=std::numeric_limits<std::int32_t>::max();break;
            case 2:values[i]=0;break;
            case 3:values[i]=-1;break;
            case 4:values[i]=1;break;
            case 5:values[i]=100001;break;
            case 6:values[i]=-100001;break;
            default:values[i]=std::bit_cast<std::int32_t>(random);break;
            }
        }
        const auto raw_path=directory/"data.i32";
        std::ofstream stream(raw_path,std::ios::binary);
        for(const auto value:values) {
            auto bits=std::bit_cast<std::uint32_t>(value);
            for(unsigned byte=0;byte<4;++byte){stream.put(static_cast<char>(bits&255));bits>>=8;}
        }
        stream.close();if(!stream)throw std::runtime_error("range test raw write");
        const auto packed_path=directory/"data.sfp";
        (void)suffice::pack_file(raw_path,packed_path,channels,block);
        raw=std::make_unique<suffice::RawDataset>(raw_path,channels);
        packed=std::make_unique<suffice::PackedDataset>(packed_path);packed->verify_payload();
    }
    ~Fixture(){packed.reset();raw.reset();std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
    std::vector<Bounds> ranges() const {
        std::vector<Bounds> result;for(const auto range:packed->info().ranges)result.push_back({range.min,range.max});return result;
    }
};

std::vector<std::uint64_t> scalar_reference(const Fixture& data,const suffice::CompiledProgram& plan) {
    std::vector<std::uint64_t> mask(suffice::mask_words(data.rows));
    std::array<i64,suffice::max_program_nodes> inputs{};
    for(std::uint64_t row=0;row<data.rows;++row) {
        std::size_t load_index=0;
        for(const auto& load:plan.loads()) {
            const auto index=std::clamp<i64>(static_cast<i64>(row)+load.offset,0,static_cast<i64>(data.rows)-1);
            inputs[load_index++]=data.values[static_cast<std::size_t>(index)*data.channels+load.channel];
        }
        if(plan.evaluate_exact(std::span<const i64>(inputs.data(),load_index))>plan.threshold())
            mask[static_cast<std::size_t>(row/64)]|=std::uint64_t{1}<<(row%64);
    }
    return mask;
}

using Slice=std::pair<std::uint64_t,std::uint64_t>;
std::vector<Slice> slices(std::uint64_t rows) {
    std::vector<Slice> result{{0,rows},{0,0},{rows,0}};
    if(rows<=17) {
        for(std::uint64_t begin=0;begin<=rows;++begin)
            for(std::uint64_t count=0;count<=rows-begin;++count)result.emplace_back(begin,count);
        return result;
    }
    constexpr std::array<std::uint64_t,25> points{{0,1,2,7,8,15,16,17,30,31,32,33,61,63,64,65,127,128,129,255,4095,4096,4097,16383,16384}};
    constexpr std::array<std::uint64_t,13> lengths{{0,1,2,7,8,31,32,63,64,65,127,257,4097}};
    for(const auto first:points)if(first<=rows)
        for(const auto count:lengths)if(count<=rows-first)result.emplace_back(first,count);
    for(const auto count:lengths)if(count<=rows)result.emplace_back(rows-count,count);
    if(rows>64){result.emplace_back(1,rows-2);result.emplace_back(31,rows-32);result.emplace_back(63,rows-64);}
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}

template<class Executor>
void compare_ranges(Executor& executor,const std::vector<std::uint64_t>& full,std::uint64_t rows,
                    const std::vector<Slice>& ranges) {
    std::vector<std::uint64_t> whole(full.size(),guard);
    const auto all_stats=executor.run(whole);
    check(whole==full,"full run changed or differs from original IR");
    check(all_stats.rows==rows,"full-run row count");
    for(const auto& [first,count]:ranges) {
        const auto words=suffice::mask_words(count);
        std::vector<std::uint64_t> storage(words+2,guard),expected(words);
        for(std::uint64_t i=0;i<count;++i)
            if((full[static_cast<std::size_t>((first+i)/64)]>>((first+i)%64))&1)
                expected[static_cast<std::size_t>(i/64)]|=std::uint64_t{1}<<(i%64);
        auto output=std::span<std::uint64_t>(storage).subspan(1,words);
        allocation_probe::start();
        const auto stats=executor.run_range(first,count,output);
        const auto allocations=allocation_probe::stop();
        check(allocations==0,"range execution allocated");
        check(storage.front()==guard&&storage.back()==guard,"range output overran its span");
        check(std::equal(output.begin(),output.end(),expected.begin(),expected.end()),"slice differs from global original-IR result");
        check(stats.rows==count&&stats.output_bytes==words*8,"range row/output accounting");
        check(stats.accepted==suffice::mask_popcount(expected),"range accepted count");
        check(std::accumulate(stats.resolved_by_bits.begin(),stats.resolved_by_bits.end(),std::uint64_t{0})==count,"range resolution histogram");
    }
    std::vector<std::uint64_t> empty,one(1,guard),extra(suffice::mask_words(rows)+1,guard);
    rejects([&]{executor.run_range(rows+1,0,empty);});
    rejects([&]{executor.run_range(rows,1,one);});
    rejects([&]{executor.run_range(0,std::numeric_limits<std::uint64_t>::max(),empty);});
    rejects([&]{executor.run_range(std::numeric_limits<std::uint64_t>::max(),1,one);});
    rejects([&]{executor.run_range(0,rows,extra);});
    rejects([&]{executor.run_range(0,0,one);});
}

void check_program(Fixture& data,const suffice::CompiledProgram& plan,std::string_view builtin={},bool predicates=true) {
    const auto reference=scalar_reference(data,plan);
    const auto ranges=slices(data.rows);
    suffice::DenseExecutor dense(*data.raw,plan,builtin,true,false);
    suffice::DenseExecutor dense_scalar(*data.raw,plan,builtin,false,true,true);
    suffice::VectorExecutor raw_vector(*data.raw,plan,63,true,true);
    suffice::VectorExecutor packed_vector(*data.packed,plan,37,true);
    suffice::VectorExecutor packed_scalar(*data.packed,plan,127,false);
    suffice::ProgressiveExecutor progressive(*data.packed,plan);
    suffice::ProgressiveExecutor generic(*data.packed,plan,{{4,8,16,32},false});
    compare_ranges(dense,reference,data.rows,ranges);
    compare_ranges(dense_scalar,reference,data.rows,ranges);
    compare_ranges(raw_vector,reference,data.rows,ranges);
    compare_ranges(packed_vector,reference,data.rows,ranges);
    compare_ranges(packed_scalar,reference,data.rows,ranges);
    compare_ranges(progressive,reference,data.rows,ranges);
    compare_ranges(generic,reference,data.rows,ranges);
    if(predicates) {
        const auto predicate=suffice::compile_predicate(plan);
        check(predicate.complete,"test predicate did not lower");
        suffice::EncodedPredicateExecutor encoded(*data.packed,predicate);
        suffice::DensePredicateExecutor raw_predicate(*data.raw,predicate,true,true);
        suffice::DensePredicateExecutor scalar_predicate(*data.raw,predicate,false);
        compare_ranges(encoded,reference,data.rows,ranges);
        compare_ranges(raw_predicate,reference,data.rows,ranges);
        compare_ranges(scalar_predicate,reference,data.rows,ranges);
    }
}

void direct_accepted_ranges(Fixture& data) {
    for(const auto& accepted:std::array<std::vector<Bounds>,5>{{
            {},{{std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()}},
            {{0,std::numeric_limits<std::int32_t>::max()}},
            {{std::numeric_limits<std::int32_t>::min(),-1}},
            {{-1000001,-700},{-2,2},{700,1000001}}}}) {
        std::vector<std::uint64_t> expected(suffice::mask_words(data.rows));
        for(std::uint64_t row=0;row<data.rows;++row) {
            bool inside=false;const auto x=data.values[static_cast<std::size_t>(row)*data.channels];
            for(const auto range:accepted)inside|=x>=range.lo&&x<=range.hi;
            if(inside)expected[static_cast<std::size_t>(row/64)]|=std::uint64_t{1}<<(row%64);
        }
        suffice::EncodedRangeExecutor executor(*data.packed,0,accepted);
        compare_ranges(executor,expected,data.rows,slices(data.rows));
    }
}
} // namespace

void test_ranges() {
    for(const auto row_count:{std::uint64_t{0},std::uint64_t{1},std::uint64_t{17},std::uint64_t{259}}) {
        Fixture data(row_count,1,32);
        for(const auto threshold:{i64{-1},i64{0},i64{100000},std::numeric_limits<i64>::max()})
            check_program(data,suffice::builtin_program("identity",threshold).compile(data.ranges()),"identity");
        check_program(data,suffice::builtin_program("fir3",100000).compile(data.ranges()),"fir3",false);
        check_program(data,suffice::builtin_program("delta",100000).compile(data.ranges()),"delta",false);
        check_program(data,suffice::builtin_program("energy",1000000).compile(data.ranges()),"energy");
        direct_accepted_ranges(data);
    }
    for(const auto block:{32u,64u,4096u}) {
        Fixture data(4197,3,block);
        const auto ranges=data.ranges();
        for(const auto source:{
            "input a 0 0\ninput b 1 0\ninput c 2 0\nmin d a b\nmax e d c\noutput e > -1\n",
            "input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 100000\n",
            "input a 0 0\ninput b 1 0\nmin d a b\nconst z 0\nsub inverse z d\nmax result inverse a\noutput result > 0\n"})
            check_program(data,suffice::parse_program(source).compile(ranges));
        const auto shifted=suffice::parse_program(
            "input left 0 -16\ninput middle 1 0\ninput right 2 16\nsub d right left\nabs a d\nmax result a middle\noutput result > 100000\n").compile(ranges);
        check_program(data,shifted,{},false);
        direct_accepted_ranges(data);
    }
    // Cross more than one 16,384-row sign tile with an unaligned request.
    Fixture large(32779,2,65536);
    const auto plan=suffice::parse_program("input a 0 0\ninput b 1 0\nmin result a b\noutput result > -1\n").compile(large.ranges());
    const auto predicate=suffice::compile_predicate(plan);
    suffice::EncodedPredicateExecutor encoded(*large.packed,predicate);
    compare_ranges(encoded,scalar_reference(large,plan),large.rows,{{1,32777},{63,16401},{16383,16395},{16385,16393}});
}
