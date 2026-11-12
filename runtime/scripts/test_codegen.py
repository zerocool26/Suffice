#!/usr/bin/env python3
"""Emit, compile, and execute differential tests of real generated C++ kernels."""

import argparse
import datetime
import json
import os
import re
from pathlib import Path

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256


FIXTURES = [
    ("positive", 2, -2147483648, 2147483647,
     "input x 0 0\ninput y 1 0\nmin both x y\noutput both > -1\n"),
    ("gate", 8, -2147483648, 2147483647,
     "input a 0 0\ninput b 1 0\ninput c 2 0\ninput d 3 0\ninput e 4 0\ninput f 5 0\ninput g 6 0\ninput h 7 0\nmin ab a b\nmin cd c d\nmax left ab cd\nmax ef e f\nmax gh g h\nmin right ef gh\nmin answer left right\noutput answer > -1\n"),
    ("magnitude", 2, -2147483648, 2147483647,
     "input x 0 0\ninput y 1 0\nabs a x\nabs b y\nmin both a b\noutput both > 100000000\n"),
    ("difference", 2, -2147483648, 2147483647,
     "input x 0 0\ninput y 1 0\nsub d x y\nabs a d\noutput a > 100000\n"),
    ("energy", 1, -2147483648, 2147483647,
     "input x 0 0\nsquare y x\noutput y > 4611686014132420609\n"),
    ("square_difference", 2, -1000000, 1000000,
     "input x 0 0\ninput y 1 0\nsub d x y\nsquare s d\noutput s > 1000000000000\n"),
    ("halo", 1, -2147483648, 2147483647,
     "input a 0 -16\ninput b 0 0\ninput c 0 16\nscale twice b 2\nsub d a twice\nadd e d c\nabs y e\noutput y > 100000\n"),
    ("affine", 2, -30000, 30000,
     "input x 0 0\ninput y 1 0\nscale a x -3\nconst bias 7\nsub b a bias\nsquare c b\nabs d y\nmax e c d\noutput e > 225000000\n"),
    ("alias", 1, -2147483648, 2147483647,
     "input x 0 0\nsquare s x\nmax m s s\nmin a m s\noutput a > 4611686014132420609\n"),
    ("min_coefficient", 1, 0, 1,
     "input x 0 0\nscale a x -9223372036854775808\noutput a > -9223372036854775807\n"),
    ("constant_true", 1, -2147483648, 2147483647,
     "const x 9223372036854775807\noutput x > -9223372036854775808\n"),
    ("constant_false", 1, -2147483648, 2147483647,
     "const x -9223372036854775808\noutput x > -9223372036854775808\n"),
    ("certificate_constant", 1, 0, 1,
     "input x 0 0\noutput x > -1\n"),
    ("wide_channels", 1024, -2147483648, 2147483647,
     "input x 1023 0\noutput x > 12345\n"),
]


TEST_BODY = r'''
#include "suffice/ir.hpp"
#include "suffice/predicate.hpp"
#include "suffice/storage.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

std::atomic<bool> track{false};
std::atomic<std::uint64_t> allocations{0};
#if defined(_MSC_VER)
#define CG_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define CG_TEST_NOINLINE __attribute__((noinline))
#else
#define CG_TEST_NOINLINE
#endif
// Keep replacement allocator boundaries visible to the optimizer. These matched
// malloc/free replacements are intentional; inlining delete into vector callers
// otherwise produces GCC's mismatched-new-delete false positive.
CG_TEST_NOINLINE void* operator new(std::size_t n) {if(track.load())allocations.fetch_add(1);if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
CG_TEST_NOINLINE void* operator new[](std::size_t n) {return ::operator new(n);}
CG_TEST_NOINLINE void operator delete(void* p) noexcept {std::free(p);}
CG_TEST_NOINLINE void operator delete[](void* p) noexcept {std::free(p);}
CG_TEST_NOINLINE void operator delete(void* p,std::size_t) noexcept {std::free(p);}
CG_TEST_NOINLINE void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#undef CG_TEST_NOINLINE

void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F&& f) {bool ok=false;try{f();}catch(const std::invalid_argument&){ok=true;}require(ok,"invalid generated contract accepted");}
std::uint64_t clamp(std::uint64_t row,int offset,std::uint64_t rows) {
    if(offset<0){const auto n=std::uint64_t(-offset);return row<n?0:row-n;}
    const auto n=std::uint64_t(offset);return n>=rows||row>=rows-n?rows-1:row+n;
}
std::vector<std::uint64_t> reference(const suffice::CompiledProgram& p,const std::vector<std::int32_t>& values,
                                   std::uint64_t rows,std::uint32_t channels,std::uint64_t first,std::uint64_t count) {
    std::vector<std::uint64_t> out(static_cast<std::size_t>(count/64+(count%64!=0)));
    std::array<suffice::i64,suffice::max_program_nodes> loaded{};
    for(std::uint64_t local=0;local<count;++local) {
        for(std::size_t j=0;j<p.loads().size();++j) loaded[j]=values[clamp(first+local,p.loads()[j].offset,rows)*channels+p.loads()[j].channel];
        if(p.evaluate_exact(std::span<const suffice::i64>(loaded.data(),p.loads().size()))>p.threshold())out[local/64]|=std::uint64_t{1}<<(local%64);
    }
    return out;
}
std::uint64_t mask_checks=0,allocation_checks=0;
template<class Kernel> void exercise(const char* text,std::uint32_t channels,std::int64_t minimum,std::int64_t maximum,bool planar=false) {
    std::vector<suffice::Bounds> ranges(channels,{minimum,maximum});
    const auto plan=suffice::parse_program(text).compile(ranges);
    constexpr std::array<std::uint64_t,17> sizes{{0,1,2,3,4,7,8,15,31,32,63,64,65,127,129,257,1031}};
    for(const auto rows:sizes) {
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows*channels));
        std::uint64_t state=0x13a2beed;
        const auto width=std::uint64_t(maximum-minimum)+1;
        for(std::size_t i=0;i<values.size();++i) {
            state^=state<<13;state^=state>>7;state^=state<<17;
            std::int64_t value=minimum+std::int64_t(state%width);
            if(i%13==0)value=minimum;
            else if(i%13==1)value=maximum;
            else if(i%13==2)value=std::clamp<std::int64_t>(0,minimum,maximum);
            else if(i%13==3)value=std::clamp<std::int64_t>(-1,minimum,maximum);
            else if(i%13==4)value=std::clamp<std::int64_t>(1,minimum,maximum);
            values[i]=static_cast<std::int32_t>(value);
        }
        auto native=values;
        if(planar)for(std::uint64_t row=0;row<rows;++row)for(std::uint32_t channel=0;channel<channels;++channel)
            native[std::uint64_t(channel)*rows+row]=values[row*channels+channel];
        Kernel scalar(native,rows,channels,false),simd(native,rows,channels,true);
        std::vector<std::uint64_t> starts{0,rows/2,rows};
        for(auto first:{1u,3u,7u,31u,63u,64u})if(first<=rows)starts.push_back(first);
        for(const auto first:starts) {
            std::vector<std::uint64_t> counts{0,rows-first};
            for(auto count:{1u,3u,4u,7u,8u,31u,63u,64u,65u})if(count<=rows-first)counts.push_back(count);
            for(const auto count:counts) {
                const auto expected=reference(plan,values,rows,channels,first,count);
                std::uint64_t expected_count=0;for(auto word:expected)expected_count+=std::popcount(word);
                std::vector<std::uint64_t> storage(expected.size()+2,0xa5719bfff32a6c17ULL);
                auto out=std::span<std::uint64_t>(storage).subspan(1,expected.size());
                for(const auto* kernel:{&scalar,&simd}) {
                    allocations.store(0);track.store(true);
                    const auto accepted=kernel->run_range(first,count,out);
                    track.store(false);require(allocations.load()==0,"generated run allocated");++allocation_checks;
                    require(accepted==expected_count&&std::equal(out.begin(),out.end(),expected.begin()),"generated kernel differs from original IR");
                    require(storage.front()==0xa5719bfff32a6c17ULL&&storage.back()==0xa5719bfff32a6c17ULL,"generated mask overflow");++mask_checks;
                }
            }
        }
        std::vector<std::uint64_t> out(static_cast<std::size_t>(rows/64+(rows%64!=0)));
        rejects([&]{(void)scalar.run_range(rows+1,0,out);});
        rejects([&]{(void)scalar.run_range(0,std::numeric_limits<std::uint64_t>::max(),out);});
        std::vector<std::uint64_t> wrong(out.size()+1);
        rejects([&]{(void)scalar.run(wrong);});
        if(rows) {
            rejects([&]{Kernel invalid(native,rows-1,channels);});
            if(values.size()*sizeof(std::int32_t)>=out.size()*sizeof(std::uint64_t)) {
                auto alias=std::span<std::uint64_t>(reinterpret_cast<std::uint64_t*>(native.data()),out.size());
                rejects([&]{(void)scalar.run(alias);});
            }
            if(maximum<std::numeric_limits<std::int32_t>::max()) {
                auto invalid_values=native;invalid_values[0]=static_cast<std::int32_t>(maximum+1);
                if(!plan.loads().empty()&&plan.loads()[0].channel==0) rejects([&]{Kernel invalid(invalid_values,rows,channels);});
            }
        }
        rejects([&]{Kernel invalid(native,rows,0);});
    }
}

std::filesystem::path packed_test_directory;
std::uint64_t packed_case_number=0,encoded_mask_checks=0,encoded_scalar_mask_checks=0,
              encoded_vector_path_checks=0,certificate_rejections=0;
std::uint64_t frontier_bound_checks=0,frontier_resolved_words=0,frontier_uncertain_words=0;
std::uint64_t residual_boundary_checks=0,residual_vector_boundary_checks=0;
struct ResidualBoundary { std::uint64_t must,may,exact; };
template<class Kernel> void exercise_encoded(const char* text,std::uint32_t channels,std::int64_t minimum,
                                             std::int64_t maximum,bool expected_simd) {
    std::vector<suffice::Bounds> ranges(channels,{minimum,maximum});
    const auto plan=suffice::parse_program(text).compile(ranges);
    for(const std::uint64_t rows:{0u,1u,7u,31u,32u,63u,65u,129u,256u,257u,512u,1031u}) {
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows*channels));
        const auto width=std::uint64_t(maximum-minimum)+1;
        std::uint64_t state=0xacef3327;
        for(std::size_t i=0;i<values.size();++i) {
            state^=state<<13;state^=state>>7;state^=state<<17;
            auto v=minimum+std::int64_t(state%width);
            if(i%7==0)v=minimum;else if(i%7==1)v=maximum;
            values[i]=static_cast<std::int32_t>(v);
        }
        const auto base=packed_test_directory/("case_"+std::to_string(packed_case_number++));
        const auto raw_name=base.string()+".i32";
        {
            std::ofstream file(raw_name,std::ios::binary);require(bool(file),"cannot create encoded fixture raw data");
            for(const auto value:values) {
                const auto bits=std::bit_cast<std::uint32_t>(value);
                for(unsigned byte=0;byte<4;++byte)file.put(static_cast<char>(bits>>(byte*8)));
            }
        }
        for(const std::uint32_t block_rows:{32u,64u,256u,4096u}) {
            const auto packed_name=base.string()+"_"+std::to_string(block_rows)+".sfp";
            (void)suffice::pack_file(raw_name,packed_name,channels,block_rows);
            {
                suffice::PackedDataset data(packed_name);
                Kernel kernel(data);
                Kernel scalar(data,true,false);
                require(!scalar.uses_simd(),"generated encoded scalar option ignored");
                require(kernel.uses_simd()==(expected_simd && block_rows>=256),"generated encoded dispatch capability mismatch");
                std::vector<std::uint64_t> starts{0,rows/2,rows};
                for(auto first:{1u,3u,7u,8u,24u,31u,63u,64u,255u,256u,257u})if(first<=rows)starts.push_back(first);
                for(const auto first:starts) {
                    std::vector<std::uint64_t> counts{0,rows-first};
                    for(auto count:{1u,7u,31u,64u,65u,255u,256u,257u,511u,512u})if(count<=rows-first)counts.push_back(count);
                    for(const auto count:counts) {
                        const auto expected=reference(plan,values,rows,channels,first,count);
                        std::uint64_t expected_count=0;for(const auto word:expected)expected_count+=std::popcount(word);
                        std::vector<std::uint64_t> storage(expected.size()+2,0xead49bff33687716ULL);
                        auto mask=std::span<std::uint64_t>(storage).subspan(1,expected.size());
                        for(auto* candidate:{&scalar,&kernel}) {
                            std::fill(mask.begin(),mask.end(),0xdeadbeefcafefeedULL);
                            allocations.store(0);track.store(true);
                            const auto accepted=candidate->run_range(first,count,mask);
                            track.store(false);require(allocations.load()==0,"generated encoded run allocated");++allocation_checks;
                            require(accepted==expected_count&&std::equal(mask.begin(),mask.end(),expected.begin()),"generated encoded differs from original IR");
                            require(storage.front()==0xead49bff33687716ULL&&storage.back()==0xead49bff33687716ULL,"generated encoded mask overflow");
                            ++encoded_mask_checks;++mask_checks;
                            if(candidate==&scalar)++encoded_scalar_mask_checks;
                            else if(kernel.uses_simd() && first%8==0 && count>=256 &&
                                    std::min<std::uint64_t>(count,block_rows-first%block_rows)>=256)
                                ++encoded_vector_path_checks;
                        }
                    }
                }
                std::vector<std::uint64_t> wrong(static_cast<std::size_t>(rows/64+(rows%64!=0))+1);
                rejects([&]{(void)kernel.run(wrong);});
                rejects([&]{(void)kernel.run_range(rows+1,0,{});});
            }
            std::filesystem::remove(packed_name);
        }
        std::filesystem::remove(raw_name);
    }
    if(!plan.loads().empty() && (minimum>std::numeric_limits<std::int32_t>::min() || maximum<std::numeric_limits<std::int32_t>::max())) {
        std::vector<std::int32_t> bad_values(channels,static_cast<std::int32_t>(minimum));
        bad_values[plan.loads()[0].channel]=static_cast<std::int32_t>(minimum>std::numeric_limits<std::int32_t>::min()?minimum-1:maximum+1);
        const auto base=packed_test_directory/("contract_"+std::to_string(packed_case_number++));
        const auto raw_name=base.string()+".i32",packed_name=base.string()+".sfp";
        {
            std::ofstream file(raw_name,std::ios::binary);
            for(const auto value:bad_values) {
                const auto bits=std::bit_cast<std::uint32_t>(value);
                for(unsigned byte=0;byte<4;++byte)file.put(static_cast<char>(bits>>(8*byte)));
            }
        }
        (void)suffice::pack_file(raw_name,packed_name,channels,32);
        {
            suffice::PackedDataset data(packed_name);data.verify_payload();
            rejects([&]{Kernel invalid(data);});++certificate_rejections;
            rejects([&]{Kernel invalid(data,true);});++certificate_rejections;
            rejects([&]{Kernel invalid(data,true,false);});++certificate_rejections;
        }
        std::filesystem::remove(raw_name);std::filesystem::remove(packed_name);
    }
}

// Boundary-focused datasets keep many rows inside the same coarse prefix cell.
// The oracle executes the original integer IR, independently of prefix lowering.
template<class Kernel,class Contracts,class Prefix,class VectorBoundary=std::nullptr_t>
void exercise_frontier_focus(const char* text,std::uint32_t channels,std::int64_t minimum,
                             std::int64_t maximum,unsigned depth,const Contracts& contracts,Prefix prefix,
                             VectorBoundary vector_boundary=nullptr,bool vector_boundary_enabled=false) {
    constexpr std::uint64_t rows=769;
    const auto plan=suffice::parse_program(text).compile(std::vector<suffice::Bounds>(channels,{minimum,maximum}));
    const auto predicate=suffice::compile_predicate(plan);
    std::vector<std::vector<std::int32_t>> thresholds(channels);
    for(unsigned i=0;i<predicate.node_count;++i)if(predicate.nodes[i].op==suffice::PredicateOp::Greater)
        thresholds[predicate.loads[predicate.nodes[i].load_index].channel].push_back(predicate.nodes[i].threshold);
    for(unsigned pattern=0;pattern<4;++pattern) {
        std::vector<std::int32_t> values(rows*channels);
        std::uint64_t state=0x932ec637;
        const auto low=depth==0?std::uint32_t{0xffffffffU}:depth==32?0U:((std::uint32_t{1}<<(32-depth))-1);
        for(std::uint64_t row=0;row<rows;++row)for(std::uint32_t channel=0;channel<channels;++channel) {
            state^=state<<13;state^=state>>7;state^=state<<17;
            const auto& ts=thresholds[channel];
            const std::int64_t threshold=ts.empty()?std::clamp<std::int64_t>(0,minimum,maximum):ts[(row/3+channel)%ts.size()];
            std::int64_t value=threshold;
            if(pattern==0)value+=std::int64_t(row%3)-1;
            else if(pattern==1)value=(row+channel)%2?minimum:maximum;
            else {
                const auto biased=std::uint32_t(std::int32_t(threshold))^0x80000000U;
                const auto suffix=pattern==2?((row&1)?low:0U):(static_cast<std::uint32_t>(state)&low);
                const auto bits=((biased&~low)|suffix)^0x80000000U;
                value=std::bit_cast<std::int32_t>(bits);
            }
            values[row*channels+channel]=static_cast<std::int32_t>(std::clamp(value,minimum,maximum));
        }
        const auto base=packed_test_directory/("frontier_"+std::to_string(packed_case_number++));
        const auto raw=base.string()+".i32";
        {
            std::ofstream stream(raw,std::ios::binary);require(bool(stream),"cannot create frontier fixture");
            for(const auto value:values) {
                const auto bits=std::bit_cast<std::uint32_t>(value);
                for(unsigned byte=0;byte<4;++byte)stream.put(static_cast<char>(bits>>(byte*8)));
            }
        }
        for(const std::uint32_t block_rows:{32u,256u,4096u}) {
            const auto name=base.string()+"_"+std::to_string(block_rows)+".sfp";
            (void)suffice::pack_file(raw,name,channels,block_rows);
            {
                suffice::PackedDataset data(name);data.verify_payload();
                Kernel scalar(data,true,false),vector(data,true,true);
                for(const std::uint64_t first:{0u,1u,7u,8u,63u,64u,255u,256u,511u,769u}) {
                    std::vector<std::uint64_t> counts{0,rows-first};
                    for(const auto count:{1u,63u,64u,255u,256u,257u})if(count<=rows-first)counts.push_back(count);
                    for(const auto count:counts) {
                        const auto expected=reference(plan,values,rows,channels,first,count);
                        std::uint64_t accepted=0;for(const auto word:expected)accepted+=std::popcount(word);
                        std::vector<std::uint64_t> guarded(expected.size()+2,0xfa17b36c4dd86295ULL);
                        auto mask=std::span<std::uint64_t>(guarded).subspan(1,expected.size());
                        for(auto* kernel:{&scalar,&vector}) {
                            std::fill(mask.begin(),mask.end(),UINT64_MAX);
                            allocations.store(0);track.store(true);
                            const auto count_actual=kernel->run_range(first,count,mask);
                            track.store(false);require(allocations.load()==0,"frontier execution allocated");++allocation_checks;
                            require(count_actual==accepted && std::equal(mask.begin(),mask.end(),expected.begin()),"frontier boundary output differs from original IR");
                            require(guarded.front()==0xfa17b36c4dd86295ULL && guarded.back()==0xfa17b36c4dd86295ULL,"frontier output guard overwritten");
                            ++mask_checks;++encoded_mask_checks;if(kernel==&scalar)++encoded_scalar_mask_checks;
                        }
                    }
                }
                std::array<std::span<const std::uint8_t>,std::tuple_size_v<Contracts>> planes{};
                for(std::uint64_t block=0;block*block_rows<rows;++block) {
                    const auto length=data.block_length(block);
                    for(std::size_t i=0;i<contracts.size();++i)planes[i]=data.plane_view(block,contracts[i].channel,contracts[i].bit,1);
                    if constexpr(requires{vector_boundary(planes,std::uint32_t{0});}) {
                        if(vector_boundary_enabled)for(std::uint32_t offset=0;offset+256<=length;offset+=256) {
                            const auto wanted=reference(plan,values,rows,channels,block*block_rows+offset,256);
                            const auto actual=vector_boundary(planes,offset);
                            require(std::equal(wanted.begin(),wanted.end(),actual.begin()),"AVX2 suffix helper with unavailable prefix views differs from original IR");
                            ++residual_vector_boundary_checks;
                        }
                    }
                    for(std::uint32_t offset=0;offset<length;offset+=64) {
                        const auto count=std::min<unsigned>(64,length-offset);
                        const auto wanted=reference(plan,values,rows,channels,block*block_rows+offset,count)[0];
                        const auto b=prefix(planes,offset,count);
                        const auto valid=count==64?UINT64_MAX:(std::uint64_t{1}<<count)-1;
                        require((b.must&~wanted)==0 && (wanted&~b.may)==0 && (b.must&~b.may)==0,"generated prefix certificate is unsound");
                        require(((b.must|b.may)&~valid)==0,"generated prefix tail is not cleared");
                        if constexpr(requires{b.exact;}) {
                            require(b.exact==wanted,"suffix-only helper with unavailable prefix views differs from original IR");
                            ++residual_boundary_checks;
                        }
                        ++frontier_bound_checks;
                        if(b.must==b.may)++frontier_resolved_words;else ++frontier_uncertain_words;
                    }
                }
            }
            std::filesystem::remove(name);
        }
        std::filesystem::remove(raw);
    }
}

#if defined(CG_TEST_BASE_SUITE) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
void check_transpose_lanes() {
    std::array<std::int32_t,128> two{};
    std::array<std::int32_t,512> eight{};
    for(std::size_t row=0;row<64;++row) {
        for(std::size_t channel=0;channel<2;++channel)two[row*2+channel]=static_cast<std::int32_t>(1000*row+channel);
        for(std::size_t channel=0;channel<8;++channel)eight[row*8+channel]=static_cast<std::int32_t>(1000*row+channel);
    }
    for(const auto first:{0u,1u,7u,31u}) {
        const auto t=cg_0_lowered::load_two_channels(two.data(),first);
        __m256i tv[2]{t.c0,t.c1};
        const auto e=cg_1_lowered::load_eight_channels(eight.data(),first);
        __m256i ev[8]{e.c0,e.c1,e.c2,e.c3,e.c4,e.c5,e.c6,e.c7};
        std::array<std::int32_t,8> output{};
        for(unsigned channel=0;channel<2;++channel) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output.data()),tv[channel]);
            for(unsigned lane=0;lane<8;++lane)require(output[lane]==two[(first+lane)*2+channel],"two-channel transpose lane mismatch");
        }
        for(unsigned channel=0;channel<8;++channel) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output.data()),ev[channel]);
            for(unsigned lane=0;lane<8;++lane)require(output[lane]==eight[(first+lane)*8+channel],"eight-channel transpose lane mismatch");
        }
    }
}
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--suite", choices=["base", "extended", "all"], default="all",
                        help="base preserves 0.3 fixtures; extended checks frontier, scheduling and planar raw layout")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("use a fresh output directory")
    output.mkdir(parents=True)
    config = compiler_config(args.compiler, args.sanitize)
    library, support = ensure_support(config, args.cache)
    generator = output / "emit_fixtures.cpp"
    lines = [
        '#include "suffice/codegen.hpp"', '#include "suffice/ir.hpp"', '#include <filesystem>',
        '#include <fstream>', '#include <stdexcept>', '#include <vector>',
        'int main(int argc,char** argv) { if(argc!=2)return 2; const std::filesystem::path output(argv[1]);',
    ]
    includes = []
    calls = []
    specifications = []
    base_fixtures = FIXTURES if args.suite in ("base", "all") else []
    if base_fixtures:
        includes.append('#define CG_TEST_BASE_SUITE 1')
    for i, (name, channels, minimum, maximum, text) in enumerate(base_fixtures):
        lines += [f'{{ std::vector<suffice::Bounds> ranges({channels},{{{minimum}LL,{maximum}LL}});',
                  f'const auto plan=suffice::parse_program({json.dumps(text)}).compile(ranges);']
        for suffix, lowered in [("lowered", True), ("original", False)]:
            namespace = f"cg_{i}_{suffix}"
            options = f'{{.namespace_name="{namespace}",.enable_avx2=true,.lower_predicates={str(lowered).lower()}}}'
            lines += [f'{{ const auto code=suffice::emit_cpp(plan,{options});',
                      f'std::ofstream file(output / "{namespace}.hpp"); if(!file)throw std::runtime_error("cannot emit fixture");file<<code.header; }}']
            includes.append(f'#include "{namespace}.hpp"')
            specifications.append({"namespace": namespace, "fixture": name, "kind": "base", "lowered": lowered})
            calls.append(f'exercise<{namespace}::PreparedKernel>({json.dumps(text)}, {channels}, {minimum}LL, {maximum}LL);')
            if channels in (2, 8):
                calls.append(f'exercise<{namespace}::PreparedKernel>({json.dumps(text)}, {channels+1}, {minimum}LL, {maximum}LL);')
            if lowered:
                enabled = f'{namespace}::encoded_avx2_emitted && {namespace}::avx2_available() && std::endian::native==std::endian::little'
                calls.append(f'if constexpr({namespace}::encoded_emitted) exercise_encoded<{namespace}::PreparedEncodedKernel<suffice::PackedDataset>>({json.dumps(text)}, {channels}, {minimum}LL, {maximum}LL, {enabled});')
        lines.append('}')

    def emit_extra(namespace, fixture, options, kind, depth=None):
        name, channels, minimum, maximum, text = fixture
        lines.extend([
            f'{{ std::vector<suffice::Bounds> ranges({channels},{{{minimum}LL,{maximum}LL}});',
            f'const auto plan=suffice::parse_program({json.dumps(text)}).compile(ranges);',
            f'const auto code=suffice::emit_cpp(plan,{{.namespace_name="{namespace}",{options}}});',
            f'std::ofstream file(output / "{namespace}.hpp"); if(!file)throw std::runtime_error("cannot emit fixture");file<<code.header;',
        ])
        if kind == "frontier":
            lines.append('if(!code.encoded_frontier_emitted)throw std::runtime_error("frontier fixture did not lower completely");')
        if kind == "residual":
            lines.append('if(!code.encoded_residual_emitted)throw std::runtime_error("residual fixture did not lower completely");')
        lines.append('}')
        includes.append(f'#include "{namespace}.hpp"')
        specifications.append({"namespace": namespace, "fixture": name, "kind": kind, "prefix_bits": depth})
        arguments = f'{json.dumps(text)}, {channels}, {minimum}LL, {maximum}LL'
        if kind in ("pressure", "planar"):
            planar = ",true" if kind == "planar" else ""
            calls.append(f'exercise<{namespace}::PreparedKernel>({arguments}{planar});')
            if channels in (2, 8):
                calls.append(f'exercise<{namespace}::PreparedKernel>({json.dumps(text)}, {channels+1}, {minimum}LL, {maximum}LL{planar});')
        if kind == "pressure" or (kind in ("frontier", "residual") and (name in ("certificate_constant", "min_coefficient", "affine") or (name == "magnitude" and depth == 12))):
            enabled = f'{namespace}::encoded_avx2_emitted && {namespace}::avx2_available() && std::endian::native==std::endian::little'
            calls.append(f'if constexpr({namespace}::encoded_emitted) exercise_encoded<{namespace}::PreparedEncodedKernel<suffice::PackedDataset>>({arguments}, {enabled});')
        if kind == "frontier":
            calls.append(f'exercise_frontier_focus<{namespace}::PreparedEncodedKernel<suffice::PackedDataset>>('
                         f'{arguments}, {depth}, {namespace}::plane_contracts,'
                         f'[](const auto& planes,std::uint32_t offset,unsigned count){{return {namespace}::evaluate_encoded_prefix_unchecked(planes,offset,count);}});')
        if kind == "residual":
            calls.append(f'exercise_frontier_focus<{namespace}::PreparedEncodedKernel<suffice::PackedDataset>>('
                         f'{arguments}, {depth}, {namespace}::plane_contracts,'
                         f'[](const auto& planes,std::uint32_t offset,unsigned count){{'
                         f'const auto evidence={namespace}::evaluate_encoded_prefix_unchecked(planes,offset,count);'
                         f'auto suffix=planes;for(std::size_t i=0;i<suffix.size();++i)'
                         f'if({namespace}::plane_contracts[i].bit<{depth})suffix[i]={{}};'
                         f'const auto exact={namespace}::evaluate_encoded_residual_unchecked(suffix,offset,count,evidence);'
                         f'return ResidualBoundary{{evidence.must,evidence.may,exact}};}},'
                         f'{namespace}_boundary_avx2,{namespace}::encoded_avx2_emitted && {namespace}::avx2_available());')

    if args.suite in ("extended", "all"):
        for i in (0, 1, 2, 7, 8, 9, 12, 13):
            emit_extra(f'pressure_{i}', FIXTURES[i], '.encoded_schedule=suffice::EncodedSchedule::Pressure', "pressure")
        for i in (0, 1, 3, 5, 6, 7, 12, 13):
            emit_extra(f'planar_{i}', FIXTURES[i], '.raw_layout=suffice::RawLayout::Planar', "planar")
        frontiers = [(FIXTURES[2], depth) for depth in (0, 1, 8, 12, 16, 31, 32)]
        frontiers += [(FIXTURES[i], depth) for i, depth in ((0,1),(1,8),(7,12),(8,31),(9,0),(12,8),(11,12))]
        for name, filename, channels, depths in [
            ("mixed_three", "predicate_mixed_three_channels.suf", 3, (12,16)),
            ("overlapping", "predicate_overlapping_gates.suf", 2, (8,12,16)),
        ]:
            fixture = (name, channels, -2147483648, 2147483647, (ROOT / "examples" / filename).read_text())
            frontiers.extend((fixture, depth) for depth in depths)
        for index, (fixture, depth) in enumerate(frontiers):
            emit_extra(f'frontier_{index}', fixture,
                       '.encoded_strategy=suffice::EncodedStrategy::Frontier,'
                       f'.encoded_prefix_bits={depth},.encoded_schedule=suffice::EncodedSchedule::Pressure',
                       "frontier", depth)
        emit_extra('frontier_legacy', FIXTURES[2],
                   '.encoded_strategy=suffice::EncodedStrategy::Frontier,.encoded_prefix_bits=12',
                   "frontier", 12)
        # Every production prefix depth receives actual compiled scalar and
        # dispatched-vector differential coverage. Broader DAG fixtures then
        # exercise shared evidence and physical batch boundaries.
        atomic = ("residual_atomic", 1, -2147483648, 2147483647,
                  "input x 0 0\noutput x > 123456789\n")
        residuals = [(atomic, depth) for depth in range(33)] + frontiers
        for index, (fixture, depth) in enumerate(residuals):
            emit_extra(f'residual_{index}', fixture,
                       '.encoded_strategy=suffice::EncodedStrategy::Residual,'
                       f'.encoded_prefix_bits={depth},.encoded_schedule=suffice::EncodedSchedule::Pressure',
                       "residual", depth)
        emit_extra('residual_legacy', FIXTURES[2],
                   '.encoded_strategy=suffice::EncodedStrategy::Residual,.encoded_prefix_bits=12',
                   "residual", 12)
    lines.append('}')
    generator.write_text("\n".join(lines) + "\n", encoding="utf-8")
    generator_exe = output / ("emit_fixtures.exe" if config["msvc"] else "emit_fixtures")
    generator_compile = compile_program(config, generator, generator_exe, library, output / "generator_compile.log")
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
    execute([generator_exe, output], output / "generation.log", env=environment)
    boundary_helpers = []
    suffix_structure_checks = 0
    for specification in specifications:
        if specification["kind"] != "residual":
            continue
        namespace = specification["namespace"]
        depth = specification["prefix_bits"]
        header = (output / f"{namespace}.hpp").read_text(encoding="utf-8")
        contract_text = header.split(" plane_contracts{{\n", 1)[1].split("\n}};", 1)[0]
        contracts = [(int(c), int(b)) for c, b in re.findall(r"\{(\d+)u,(\d+)u\}", contract_text)]
        for function in ("evaluate_encoded_residual_unchecked", "evaluate_encoded_residual_avx2_unchecked"):
            if function + "(" not in header:
                continue
            body = header.split(function + "(", 1)[1].split("\n}\n", 1)[0]
            for index in map(int, re.findall(r"planes\[(\d+)\]", body)):
                if contracts[index][1] < depth:
                    raise RuntimeError(f"{namespace}/{function} reads forbidden prefix plane {contracts[index]}")
            suffix_structure_checks += 1
        signature = (f'std::array<std::uint64_t,4> {namespace}_boundary_avx2('
                     f'const std::array<std::span<const std::uint8_t>,{namespace}::encoded_plane_count>& planes,'
                     'std::uint32_t offset)')
        if "evaluate_encoded_residual_avx2_unchecked(" in header:
            boundary_helpers.extend([
                '#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)',
                '#if defined(__GNUC__) || defined(__clang__)',
                '__attribute__((target("avx2")))', '#endif', signature + '{',
                f'const auto evidence={namespace}::evaluate_encoded_prefix_avx2_unchecked(planes,offset);',
                'auto suffix=planes;for(std::size_t i=0;i<suffix.size();++i)',
                f'if({namespace}::plane_contracts[i].bit<{depth})suffix[i]={{}};',
                f'const auto exact={namespace}::evaluate_encoded_residual_avx2_unchecked(suffix,offset,evidence);',
                'std::array<std::uint64_t,4> result{};_mm256_storeu_si256(reinterpret_cast<__m256i*>(result.data()),exact);return result;}',
                '#else', signature + '{(void)planes;(void)offset;throw std::runtime_error("unavailable AVX2 boundary probe");}', '#endif'])
        else:
            boundary_helpers.append(signature + '{(void)planes;(void)offset;throw std::runtime_error("constant residual has no AVX2 boundary");}')
    test = output / "generated_tests.cpp"
    test.write_text("\n".join(includes) + "\n" + TEST_BODY + "\n" + "\n".join(boundary_helpers) + '\nint main(int argc,char** argv){try{\nif(argc!=2){return 2;}\npacked_test_directory=argv[1];std::filesystem::create_directories(packed_test_directory);\n'
                    '#if defined(CG_TEST_BASE_SUITE) && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))\n'
                    'if(cg_0_lowered::avx2_available())check_transpose_lanes();\n#endif\n' +
                    "\n".join(calls) +
                    '\nstd::cout<<"{\\\"exact\\\":true,\\\"mask_checks\\\":"<<mask_checks<<",\\\"encoded_mask_checks\\\":"<<encoded_mask_checks'
                    '<<",\\\"encoded_scalar_mask_checks\\\":"<<encoded_scalar_mask_checks<<",\\\"encoded_vector_path_checks\\\":"<<encoded_vector_path_checks'
                    '<<",\\\"certificate_rejections\\\":"<<certificate_rejections<<",\\\"allocation_checks\\\":"<<allocation_checks'
                    '<<",\\\"frontier_bound_checks\\\":"<<frontier_bound_checks<<",\\\"frontier_resolved_words\\\":"<<frontier_resolved_words'
                    '<<",\\\"frontier_uncertain_words\\\":"<<frontier_uncertain_words'
                    '<<",\\\"residual_boundary_checks\\\":"<<residual_boundary_checks'
                    '<<",\\\"residual_vector_boundary_checks\\\":"<<residual_vector_boundary_checks<<"}\\n";\n'
                    '}catch(const std::exception& e){track.store(false);std::cerr<<e.what()<<\'\\n\';return 1;} }\n',
                    encoding="utf-8")
    executable = output / ("generated_tests.exe" if config["msvc"] else "generated_tests")
    test_compile = compile_program(config, test, executable, library, output / "test_compile.log", include=output)
    execution_ms = execute([executable, output / "packed_fixtures"], output / "execution.log", env=environment)
    checks = json.loads((output / "execution.log").read_text(encoding="utf-8").strip())
    record = {"created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "compiler": config, "shared_support_library": support,
              "suite": args.suite, "fixture_count": len({s["fixture"] for s in specifications}),
              "generated_headers": len(specifications), "emitted_variants": specifications,
              "generator_compile": generator_compile, "test_compile": test_compile,
              "execution_wall_ms": execution_ms, "checks": checks,
              "suffix_structure_checks": suffix_structure_checks,
              "header_sha256": {p.name: sha256(p) for p in sorted(output.glob("*.hpp"))},
              "scope": "Actual compiled scalar and AVX2 raw/encoded kernels; base original/lowered fixtures plus selected pressure/planar/frontier variants; residual every depth 0..32 and shared compound DAGs; direct original-IR differential masks; independent scalar/AVX2 suffix calls with prefix plane spans removed; emitted suffix-plane access checks; boundary and equal-prefix datasets; prefix soundness; arbitrary global slices; small-block scalar fallback; cleared tails; allocation and certificate checks. LeakSanitizer disabled only for sanitizer mode."}
    (output / "result.json").write_text(json.dumps(record, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "generated_headers": record["generated_headers"],
                      "checks": checks, "sanitize": args.sanitize}, indent=2))


if __name__ == "__main__":
    main()
