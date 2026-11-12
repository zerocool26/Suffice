#include "suffice/vector.hpp"
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
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using suffice::i64;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(std::string("vector test: ")+message);}
template<class F>void must_throw(F&& f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}require(threw,"invalid contract accepted");}

struct Fixture {
    std::filesystem::path directory;
    std::vector<std::int32_t> values;
    std::unique_ptr<suffice::RawDataset> raw;
    std::unique_ptr<suffice::PackedDataset> packed;
    Fixture(std::uint32_t rows,std::uint32_t channels,std::uint32_t block) {
        static std::atomic<unsigned> counter{0};
        directory=std::filesystem::temp_directory_path()/("suffice-vector-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(counter++));
        require(std::filesystem::create_directory(directory),"temporary directory collision");
        values.resize(static_cast<std::size_t>(rows)*channels);std::uint32_t random=123456789;
        std::ofstream file(directory/"raw.i32",std::ios::binary);
        for(std::size_t i=0;i<values.size();++i){random^=random<<13;random^=random>>17;random^=random<<5;
            const std::array<std::int32_t,8> fixed{{std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),0,-1,1,100000,-100000,7}};
            values[i]=(i%13<8)?fixed[i%13]:std::bit_cast<std::int32_t>(random);
            const auto bits=std::bit_cast<std::uint32_t>(values[i]);for(unsigned b=0;b<4;++b)file.put(static_cast<char>((bits>>(8*b))&255));
        }
        file.close();require(bool(file),"cannot write fixture");
        (void)suffice::pack_file(directory/"raw.i32",directory/"packed.sfp",channels,block);
        raw=std::make_unique<suffice::RawDataset>(directory/"raw.i32",channels);
        packed=std::make_unique<suffice::PackedDataset>(directory/"packed.sfp");packed->verify_payload();
    }
    ~Fixture(){packed.reset();raw.reset();std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
    std::vector<suffice::Bounds> ranges() const {std::vector<suffice::Bounds> result;for(auto r:packed->info().ranges)result.push_back({r.min,r.max});return result;}
};

void check(Fixture& f,const suffice::CompiledProgram& plan,std::size_t tile,bool simd) {
    std::vector<std::uint64_t> reference(suffice::mask_words(f.raw->info().rows)),actual(reference.size());
    suffice::DenseExecutor oracle(*f.raw,plan);const auto expected=oracle.run(reference);
    suffice::VectorExecutor raw(*f.raw,plan,tile,simd),packed(*f.packed,plan,tile,simd);
    for(auto* executor:{&raw,&packed}) {
        auto stats=executor->run(actual);require(actual==reference,"mask differs from exact original IR");
        require(stats.accepted==expected.accepted,"accepted count mismatch");
        require(stats.peak_scratch_bytes==executor->scratch_bytes(),"scratch accounting mismatch");
        require(std::accumulate(stats.resolved_by_bits.begin(),stats.resolved_by_bits.end(),std::uint64_t{0})==stats.rows,"resolution accounting mismatch");
        require(executor->slot_count()<=plan.node_count(),"liveness assignment added slots");
        allocation_probe::start();stats=executor->run(actual);const auto allocations=allocation_probe::stop();
        require(allocations==0,"prepared run allocated");require(actual==reference,"replay mask mismatch");
    }
}

void matrix() {
    for(auto rows:{0U,1U,3U,31U,32U,33U,63U,64U,65U,127U,257U})for(auto channels:{1U,3U})for(auto block:{32U,64U,128U}) {
        Fixture f(rows,channels,block);const auto ranges=f.ranges();
        for(std::uint32_t channel=0;channel<channels;++channel) {
            for(const auto name:{"identity","delta","fir3","energy"}) {
                const auto plan=suffice::builtin_program(name,100000,channel).compile(ranges);
                for(auto tile:{1U,5U,64U,256U}) {check(f,plan,tile,true);if(tile==5)check(f,plan,tile,false);}
            }
        }
    }
}

void custom_and_lifetimes() {
    Fixture f(257,3,64);const auto ranges=f.ranges();
    for(int offset:{-16,0,16}) {
        suffice::Program p;const auto x=p.load(0,offset),y=p.load(2,-offset);
        const auto difference=p.sub(x,y);const auto affine=p.add(p.mul_const(x,3),p.constant(-1234567));
        const auto shared=p.max(p.abs(difference),p.abs(affine));
        p.output(p.add(p.min(shared,p.constant(9999999999LL)),p.min(x,y)),100000);
        const auto plan=p.compile(ranges);for(auto tile:{3U,17U,65U,256U}){check(f,plan,tile,true);check(f,plan,tile,false);}
        suffice::VectorExecutor executor(*f.packed,plan);require(executor.slot_count()<plan.node_count(),"temporary lifetimes were not reused");
    }
    // Full int32 squares stress the signed 64-bit AVX2 product implementation.
    for(i64 threshold:std::array<i64,4>{{0,1,4611686014132420609LL,4611686018427387904LL}}) {
        suffice::Program p;const auto x=p.load(1);p.output(p.square(x),threshold);
        const auto plan=p.compile(ranges);check(f,plan,5,true);check(f,plan,5,false);
    }
    // Constants, both operand aliases, and dying right operands.
    const std::array<std::string,4> programs{{
        "const x -9223372036854775808; output x > -9223372036854775808",
        "input x 0 0; scale a x -2147483648; output a > 0",
        "input x 0 0; input y 1 0; add s x y; sub d s y; add z d d; output z > -1",
        "input x 0 0; input y 1 0; min a x y; max b x y; sub r b a; output r > 100000"
    }};
    for(const auto& source:programs) {const auto plan=suffice::parse_program(source).compile(ranges);check(f,plan,5,true);check(f,plan,5,false);}
}

void contracts_and_compaction() {
    Fixture f(65,1024,32);suffice::Program p;p.output(p.load(1023),0);const auto plan=p.compile(f.ranges());
    suffice::VectorExecutor compact(*f.packed,plan,64);require(compact.scratch_bytes()<8192,"unused channels inflated decode workspace");
    check(f,plan,64,true);
    must_throw([&]{suffice::VectorExecutor e(*f.raw,plan,0);});
    must_throw([&]{suffice::VectorExecutor e(*f.packed,plan,65537);});
    must_throw([&]{compact.run({});});
    must_throw([&]{suffice::CompiledProgram empty;suffice::VectorExecutor e(*f.raw,empty);});
    const std::array<suffice::Bounds,1> narrow{{{-1,1}}};const auto invalid=suffice::builtin_program("identity",0).compile(narrow);
    must_throw([&]{suffice::VectorExecutor e(*f.raw,invalid);});
    must_throw([&]{suffice::VectorExecutor e(*f.packed,invalid);});
}
} // namespace

void test_vector(){matrix();custom_and_lifetimes();contracts_and_compaction();}
