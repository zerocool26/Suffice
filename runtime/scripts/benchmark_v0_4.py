#!/usr/bin/env python3
"""Same-process frozen/current AOT comparisons with fused raw columnar controls.

Each generated header is compiled in its own translation unit, then all wrappers
are linked into one executable. The original scalar IR remains the oracle.
Every fresh timed complete output is checked after the timer. All preparation,
copy/transpose, source emission, compilation and linking costs remain separate.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

from codegen_support import ROOT, compiler_config, ensure_support, execute, sha256


VARIANTS = (
    {"key": "frozen", "namespace": "bench_frozen03", "baseline": True,
     "flags": [], "raw": False, "encoded": True, "label": "v03-eager"},
    {"key": "eager", "namespace": "bench_eager04", "baseline": False,
     "flags": ["--encoded-strategy", "eager", "--encoded-schedule", "pressure"],
     "raw": True, "encoded": True, "label": "v04-eager-pressure"},
    {"key": "soa", "namespace": "bench_soa04", "baseline": False,
     "flags": ["--raw-layout", "planar", "--encoded-strategy", "eager", "--encoded-schedule", "pressure"],
     "raw": True, "encoded": False, "label": "v04-raw-soa"},
    *({"key": f"frontier{depth}", "namespace": f"bench_frontier{depth}", "baseline": False,
       "flags": ["--encoded-strategy", "frontier", "--encoded-prefix-bits", str(depth),
                 "--encoded-schedule", "pressure"], "raw": False, "encoded": True,
       "label": f"v04-frontier{depth}", "prefix": depth} for depth in (8, 12, 16)),
)


INTERFACE = r'''
#pragma once
#include "suffice/storage.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct PrefixDiagnostic {
    bool available{};
    std::uint64_t vector_batches{}, fallback_batches{}, certified_rows{}, ambiguous_rows{};
    std::uint64_t bounds_checked_rows{}, scalar_or_tail_rows{};
};
struct BenchRunner {
    virtual ~BenchRunner() = default;
    virtual std::uint64_t run(std::span<std::uint64_t>) = 0;
    virtual std::uint64_t run_range(std::uint64_t,std::uint64_t,std::span<std::uint64_t>) = 0;
    [[nodiscard]] virtual bool uses_simd() const noexcept = 0;
    [[nodiscard]] virtual bool simd_known() const noexcept { return true; }
    [[nodiscard]] virtual PrefixDiagnostic diagnose(const suffice::PackedDataset&,
                                                     std::span<const std::uint64_t>) const { return {}; }
};
struct Candidate {
    std::string name, representation, implementation;
    std::unique_ptr<BenchRunner> runner;
    std::vector<double> samples;
    double prepare_ms{};
    PrefixDiagnostic diagnostic;
};
using Native = std::span<const std::int32_t>;
// Variant registration declarations are generated below, with fixed safe names.
'''


WRAPPER = r'''
#include "kernel_@KEY@.hpp"
#include "benchmark_interface.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <stdexcept>

namespace {
namespace generated = @NAMESPACE@;
using Clock=std::chrono::steady_clock;
class Raw final : public BenchRunner {
    generated::PreparedKernel kernel_;
public:
    Raw(Native values,std::uint64_t rows,std::uint32_t channels,bool simd):kernel_(values,rows,channels,simd){}
    std::uint64_t run(std::span<std::uint64_t> mask) override { return kernel_.run(mask); }
    std::uint64_t run_range(std::uint64_t first,std::uint64_t count,std::span<std::uint64_t> mask) override { return kernel_.run_range(first,count,mask); }
    bool uses_simd() const noexcept override { return kernel_.uses_simd(); }
};
class Encoded final : public BenchRunner {
    generated::PreparedEncodedKernel<suffice::PackedDataset> kernel_;
public:
    explicit Encoded(const suffice::PackedDataset& data):kernel_(data,true,true){}
    std::uint64_t run(std::span<std::uint64_t> mask) override { return kernel_.run(mask); }
    std::uint64_t run_range(std::uint64_t first,std::uint64_t count,std::span<std::uint64_t> mask) override { return kernel_.run_range(first,count,mask); }
    bool uses_simd() const noexcept override { return kernel_.uses_simd(); }
@DIAGNOSTIC@
};
template<class Factory>
void add(std::vector<Candidate>& candidates,const char* name,const char* representation,
         const char* implementation,Factory&& factory) {
    const auto start=Clock::now(); auto runner=factory();
    const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    candidates.push_back({name,representation,implementation,std::move(runner),{},elapsed,{}});
}
}
void register_@KEY@(std::vector<Candidate>& candidates,[[maybe_unused]] Native aos,
                    [[maybe_unused]] Native soa,[[maybe_unused]] std::uint64_t rows,
                    [[maybe_unused]] std::uint32_t channels,[[maybe_unused]] const suffice::PackedDataset& packed) {
@REGISTER@
}
'''


DIAGNOSTIC = r'''
    PrefixDiagnostic diagnose(const suffice::PackedDataset& data,
                              std::span<const std::uint64_t> expected) const override {
        PrefixDiagnostic result; result.available=true;
        // This untimed scalar calculation reproduces the Boolean uncertainty
        // condition of each eligible 256-row vector batch. It does not count
        // hardware loads or instrument the timed implementation.
        // A zero-plane constant kernel uses its scalar output-fill fast path
        // even when the target reports that AVX2 is available.
        if(generated::encoded_plane_count==0 || !kernel_.uses_simd() || data.info().block_rows<256) {
            result.scalar_or_tail_rows=data.info().rows; return result;
        }
        std::array<std::span<const std::uint8_t>,generated::encoded_plane_count> planes{};
        for(std::uint64_t block=0;block<data.block_count();++block) {
            const auto length=data.block_length(block);
            for(std::size_t i=0;i<planes.size();++i) {
                const auto contract=generated::plane_contracts[i];
                planes[i]=data.plane_view(block,contract.channel,contract.bit,1);
            }
            const auto complete=length&~std::uint32_t{255};
            for(std::uint32_t offset=0;offset<complete;offset+=256) {
                std::uint64_t any_uncertain=0;
                for(unsigned word=0;word<4;++word) {
                    const auto bounds=generated::evaluate_encoded_prefix_unchecked(planes,offset+word*64,64);
                    const auto global=block*data.info().block_rows+offset+word*64;
                    const auto truth=expected[static_cast<std::size_t>(global/64)];
                    if((bounds.must&~bounds.may) || (bounds.must&~truth) || (truth&~bounds.may))
                        throw std::runtime_error("generated prefix diagnostic violates exact bounds");
                    const auto unknown=bounds.may^bounds.must;
                    const auto ambiguous=static_cast<unsigned>(std::popcount(unknown));
                    result.ambiguous_rows+=ambiguous;
                    result.certified_rows+=64-ambiguous;
                    result.bounds_checked_rows+=64;
                    any_uncertain|=unknown;
                }
                ++result.vector_batches;
                result.fallback_batches+=any_uncertain!=0;
            }
            result.scalar_or_tail_rows+=length-complete;
        }
        return result;
    }
'''


DRIVER = r'''
#include "benchmark_interface.hpp"
#include "suffice/ir.hpp"
#include "suffice/runtime.hpp"
#include "suffice/predicate.hpp"
#include "suffice/vector.hpp"
#include "suffice/wordcode.hpp"
#include "suffice/frontier.hpp"
#include "suffice/frontier_runtime.hpp"
#include "suffice/benchmark_order.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>

using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
template<class Executor> class Core final : public BenchRunner {
    Executor executor_;
public:
    template<class... Args> explicit Core(Args&&... args):executor_(std::forward<Args>(args)...){ }
    std::uint64_t run(std::span<std::uint64_t> mask) override { return executor_.run(mask).accepted; }
    std::uint64_t run_range(std::uint64_t first,std::uint64_t count,std::span<std::uint64_t> mask) override { return executor_.run_range(first,count,mask).accepted; }
    bool uses_simd() const noexcept override {
        if constexpr(requires { executor_.uses_simd(); })return executor_.uses_simd();
        else return false;
    }
    bool simd_known() const noexcept override { return requires { executor_.uses_simd(); }; }
};
template<class Executor,class... Args>
void add_core(std::vector<Candidate>& candidates,const char* name,const char* representation,Args&&... args) {
    const auto begin=Clock::now();auto runner=std::make_unique<Core<Executor>>(std::forward<Args>(args)...);
    const auto elapsed=ms(begin);
    candidates.push_back({name,representation,"core",std::move(runner),{},elapsed,{}});
}
struct Aligned {
    std::int32_t* p{}; std::size_t count{};
    explicit Aligned(std::size_t n):p(static_cast<std::int32_t*>(::operator new[](n*sizeof(std::int32_t),std::align_val_t{64}))),count(n){}
    ~Aligned(){::operator delete[](p,std::align_val_t{64});}
    Aligned(const Aligned&)=delete;Aligned& operator=(const Aligned&)=delete;
    std::span<std::int32_t> span() { return {p,count}; }
};
std::vector<std::uint64_t> slice(std::span<const std::uint64_t> full,std::uint64_t first,std::uint64_t count) {
    std::vector<std::uint64_t> result(suffice::mask_words(count));
    for(std::uint64_t i=0;i<count;++i)
        if((full[static_cast<std::size_t>((first+i)/64)]>>((first+i)%64))&1U)
            result[static_cast<std::size_t>(i/64)]|=std::uint64_t{1}<<(i%64);
    return result;
}

int main(int argc,char** argv) {
    try {
        if(argc!=7)throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD MIN_REPEAT OUTPUT_JSON");
        auto phase=Clock::now();
        suffice::PackedDataset packed(argv[2]);
        suffice::RawDataset raw(argv[1],packed.info().channels);
        packed.verify_payload();
        if(raw.info().rows!=packed.info().rows)throw std::invalid_argument("raw/packed dimensions disagree");
        const auto rows=raw.info().rows;
        const auto channels=raw.info().channels;
        std::vector<std::int32_t> block(packed.info().block_rows);
        for(std::uint64_t b=0;b<packed.block_count();++b)for(std::uint32_t c=0;c<channels;++c) {
            packed.decode_block(b,c,block);
            for(std::uint32_t j=0;j<packed.block_length(b);++j)
                if(block[j]!=raw.value(b*packed.info().block_rows+j,c))throw std::runtime_error("raw/packed value mismatch");
        }
        const auto validation_ms=ms(phase);
        phase=Clock::now();
        std::ifstream source(argv[3]);const bool custom=source.good();
        const auto text=custom?std::string(std::istreambuf_iterator<char>(source),{}):std::string{};
        auto program=custom?suffice::parse_program(text):suffice::builtin_program(argv[3],std::stoll(argv[4]));
        std::vector<suffice::Bounds> ranges;
        for(auto range:packed.info().ranges)ranges.push_back({range.min,range.max});
        auto plan=program.compile(ranges);
        auto predicate=suffice::compile_predicate(plan);
        auto wordcode=suffice::compile_wordcode(predicate);
        auto frontier=suffice::compile_frontier(predicate,12);
        const auto plan_ms=ms(phase);
        phase=Clock::now();
        std::vector<std::uint64_t> expected(suffice::mask_words(rows)),output(expected.size());
        suffice::DenseExecutor oracle(raw,plan,{},false,false);
        const auto truth=oracle.run(expected);
        const auto oracle_ms=ms(phase);
        phase=Clock::now();
        Aligned aos(raw.bytes().size()/4);raw.read_rows(0,rows,aos.span());
        const auto aos_copy_ms=ms(phase);
        phase=Clock::now();
        Aligned soa(aos.count);
        constexpr std::uint64_t transpose_tile=4096;
        for(std::uint64_t first=0;first<rows;first+=transpose_tile) {
            const auto end=std::min(rows,first+transpose_tile);
            for(std::uint32_t c=0;c<channels;++c)for(auto row=first;row<end;++row)
                soa.p[static_cast<std::size_t>(c)*rows+row]=aos.p[static_cast<std::size_t>(row)*channels+c];
        }
        const auto soa_transpose_ms=ms(phase);
        std::vector<Candidate> candidates;
        phase=Clock::now();
@REGISTER_VARIANTS@
        const auto generated_prepare_ms=ms(phase);
        phase=Clock::now();
        add_core<suffice::DenseExecutor>(candidates,"dense","interleaved",raw,plan,custom?std::string{}:std::string(argv[3]),true,true);
        add_core<suffice::VectorExecutor>(candidates,"raw-vector","interleaved",raw,plan);
        if(predicate.complete)add_core<suffice::EncodedPredicateExecutor>(candidates,"encoded-predicate","packed",packed,predicate);
        if(wordcode.complete)add_core<suffice::WordcodeExecutor>(candidates,"encoded-wordcode","packed",packed,wordcode);
        if(frontier.complete)add_core<suffice::FrontierExecutor>(candidates,"encoded-frontier12","packed",packed,frontier);
        const auto core_prepare_ms=ms(phase);
        const auto verify=[&](const Candidate& candidate,std::uint64_t accepted) {
            if(accepted!=truth.accepted || output!=expected)
                throw std::runtime_error("full original-IR differential mismatch: "+candidate.name);
        };
        phase=Clock::now();
        std::uint64_t range_checks=0;
        for(auto& candidate:candidates) {
            const std::array<std::uint64_t,9> starts{{0,1,7,31,63,64,255,rows>65?rows-65:rows,rows}};
            for(auto first:starts)if(first<=rows)for(auto wanted:{std::uint64_t{0},std::uint64_t{1},std::uint64_t{73},std::uint64_t{513}}) {
                const auto count=std::min(wanted,rows-first);const auto exact=slice(expected,first,count);
                std::vector<std::uint64_t> guarded(exact.size()+2,UINT64_MAX);
                auto mask=std::span<std::uint64_t>(guarded).subspan(1,exact.size());
                const auto accepted=candidate.runner->run_range(first,count,mask);
                if(guarded.front()!=UINT64_MAX || guarded.back()!=UINT64_MAX ||
                   accepted!=suffice::mask_popcount(exact) || !std::equal(mask.begin(),mask.end(),exact.begin(),exact.end()))
                    throw std::runtime_error("global-range differential mismatch: "+candidate.name);
                ++range_checks;
            }
        }
        const auto range_validation_ms=ms(phase);
        phase=Clock::now();
        for(auto& candidate:candidates)candidate.diagnostic=candidate.runner->diagnose(packed,expected);
        const auto prefix_diagnostic_ms=ms(phase);
        phase=Clock::now();
        for(auto& candidate:candidates) {
            std::fill(output.begin(),output.end(),UINT64_MAX);
            verify(candidate,candidate.runner->run(output));
        }
        const auto warmup_ms=ms(phase);
        const auto minimum=std::stoul(argv[5]);
        if(minimum<1 || minimum>1000)throw std::invalid_argument("minimum repetition count must be 1..1000");
        const auto cycle=suffice::balanced_timing_cycle_rows(candidates.size());
        const auto repeat=static_cast<unsigned>(((minimum+cycle-1)/cycle)*cycle);
        for(auto& c:candidates)c.samples.reserve(repeat);
        constexpr std::uint64_t timing_seed=0x5355464649434534ULL;
        const auto orders=suffice::balanced_timing_orders(candidates.size(),repeat,timing_seed);
        std::uint64_t checks=0,checksum=0;
        for(unsigned round=0;round<repeat;++round)for(std::size_t position=0;position<candidates.size();++position) {
            auto& candidate=candidates[orders[round][position]];
            // Equal output initialization is outside the timer and catches stale
            // output/padding errors. It also warms the output buffer for all paths.
            std::fill(output.begin(),output.end(),UINT64_MAX);
            phase=Clock::now();const auto accepted=candidate.runner->run(output);const auto elapsed=ms(phase);
            verify(candidate,accepted);
            if(!std::isfinite(elapsed) || elapsed<=0)throw std::runtime_error("invalid timing observation");
            candidate.samples.push_back(elapsed);++checks;
            checksum^=suffice::mask_checksum(output)+round+position;
        }
        std::ofstream result(argv[6]);if(!result)throw std::runtime_error("cannot create measurement JSON");
        result<<std::setprecision(12)<<"{\n\"schema_version\":4,\"rows\":"<<rows<<",\"channels\":"<<channels
              <<",\"block_rows\":"<<packed.info().block_rows<<",\"minimum_repeat\":"<<minimum
              <<",\"repeat\":"<<repeat<<",\"exact\":true,\"accepted\":"<<truth.accepted
              <<",\"mask_checksum\":"<<suffice::mask_checksum(expected)<<",\"fresh_mask_checks\":"<<checks
              <<",\"range_mask_checks\":"<<range_checks<<",\"checksum_accumulator\":"<<checksum
              <<",\"validation_ms\":"<<validation_ms<<",\"plan_ms\":"<<plan_ms<<",\"oracle_ms\":"<<oracle_ms
              <<",\"aos_copy_ms\":"<<aos_copy_ms<<",\"soa_transpose_ms\":"<<soa_transpose_ms
              <<",\"aos_copy_bytes\":"<<aos.count*4<<",\"soa_copy_bytes\":"<<soa.count*4
              <<",\"native_buffer_alignment_bytes\":64,\"soa_channel_stride_values\":"<<rows
              <<",\"generated_prepare_ms\":"<<generated_prepare_ms<<",\"core_prepare_ms\":"<<core_prepare_ms
              <<",\"range_validation_ms\":"<<range_validation_ms<<",\"prefix_diagnostic_ms\":"<<prefix_diagnostic_ms
              <<",\"warmup_ms\":"<<warmup_ms<<",\"timing_order_seed\":"<<timing_seed
              <<",\"timing_order_protocol\":\"randomized Williams complete cycles\",\"timing_cycle_rows\":"<<cycle
              <<",\"complete_timing_cycles\":"<<repeat/cycle<<",\"timing_order\":[";
        for(std::size_t i=0;i<orders.size();++i) {
            if(i)result<<',';
            result<<'[';
            for(std::size_t j=0;j<orders[i].size();++j){if(j)result<<',';result<<'\"'<<candidates[orders[i][j]].name<<'\"';}
            result<<']';
        }
        result<<"],\n\"candidates\":[\n";
        bool comma=false;
        for(const auto& c:candidates) {
            auto sorted=c.samples;std::sort(sorted.begin(),sorted.end());
            const auto median=sorted.size()%2?sorted[sorted.size()/2]:(sorted[sorted.size()/2-1]+sorted[sorted.size()/2])/2;
            if(comma)result<<",\n";
            comma=true;
            result<<"{\"backend\":\""<<c.name<<"\",\"representation\":\""<<c.representation
                  <<"\",\"implementation\":\""<<c.implementation<<"\",\"simd_available\":"
                  <<(c.runner->simd_known()?(c.runner->uses_simd()?"true":"false"):"null")
                  <<",\"prepare_ms\":"<<c.prepare_ms<<",\"median_ms\":"<<median<<",\"min_ms\":"<<sorted.front()
                  <<",\"max_ms\":"<<sorted.back()<<",\"samples_ms\":[";
            for(std::size_t j=0;j<c.samples.size();++j){if(j)result<<',';result<<c.samples[j];}
            result<<"],\"prefix_diagnostic\":{\"available\":"<<(c.diagnostic.available?"true":"false")
                  <<",\"vector_batches\":"<<c.diagnostic.vector_batches<<",\"fallback_batches\":"<<c.diagnostic.fallback_batches
                  <<",\"certified_rows\":"<<c.diagnostic.certified_rows<<",\"ambiguous_rows\":"<<c.diagnostic.ambiguous_rows
                  <<",\"bounds_checked_rows\":"<<c.diagnostic.bounds_checked_rows
                  <<",\"scalar_or_tail_rows\":"<<c.diagnostic.scalar_or_tail_rows<<"}}";
        }
        result<<"\n]}\n";
        std::cout<<"PASS "<<checks<<" fresh complete masks; "<<range_checks<<" range masks; "
                 <<repeat<<" rounds in "<<repeat/cycle<<" complete Williams cycles\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
'''


def write_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def compile_object(config, source, obj, include, log):
    if config["msvc"]:
        argv = [config["executable"], *config["flags"], "/I" + str(ROOT / "include"),
                "/I" + str(include), "/c", str(source), "/Fo" + str(obj)]
    else:
        argv = [config["executable"], *config["flags"], "-I", str(ROOT / "include"),
                "-I", str(include), "-c", str(source), "-o", str(obj)]
    wall = execute(argv, log, cwd=include)
    return {"argv": argv, "wall_ms": wall, "source_sha256": sha256(source), "object_sha256": sha256(obj)}


def wrapper_source(variant):
    registration = []
    if variant["raw"]:
        planar = variant["key"] == "soa"
        layout = "soa" if planar else "aos"
        representation = "planar" if planar else "interleaved"
        for simd in (False, True):
            name = "raw-" + layout + ("-simd" if simd else "-scalar")
            registration.append(f'    add(candidates,"{name}","{representation}","generated",[&]{{return std::make_unique<Raw>({layout},rows,channels,{str(simd).lower()});}});')
    if variant["encoded"]:
        registration.append(f'    if constexpr(generated::encoded_emitted)add(candidates,"{variant["label"]}","packed","generated",[&]{{return std::make_unique<Encoded>(packed);}});')
    return (WRAPPER.replace("@KEY@", variant["key"]).replace("@NAMESPACE@", variant["namespace"])
            .replace("@DIAGNOSTIC@", DIAGNOSTIC if "prefix" in variant else "")
            .replace("@REGISTER@", "\n".join(registration)))


def default_baseline():
    executable = "suffice.exe" if os.name == "nt" else "suffice"
    candidates = [ROOT / "baseline/v0_3/build" / executable,
                  ROOT / "baseline_v0_3/build" / executable,
                  ROOT / "history/v0_3/bin/linux-x86_64" / executable,
                  ROOT.parent / "suffice_v0_3/build" / executable]
    return next((path for path in candidates if path.is_file()), candidates[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("suffice.exe" if os.name == "nt" else "suffice"))
    parser.add_argument("--baseline-binary", type=Path, default=default_baseline())
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--packed", required=True, type=Path)
    parser.add_argument("--program", required=True)
    parser.add_argument("--threshold", type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeat", type=int, default=40, help="Minimum repetitions; rounds up to complete Williams cycles")
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--pin-cpu", default="auto", help="auto, none, or an allowed Linux CPU number")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 1000:
        parser.error("repeat must be 1..1000")
    for path in (args.binary, args.baseline_binary, args.raw, args.packed):
        if not path.is_file():
            parser.error(f"required file not found: {path}")
    output = args.output.resolve()
    if output.exists():
        parser.error("output directory exists; use a fresh path")
    available = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    if args.pin_cpu != "none" and available:
        cpu = available[0] if args.pin_cpu == "auto" else int(args.pin_cpu)
        if cpu not in available:
            parser.error("requested CPU is not allowed")
        os.sched_setaffinity(0, {cpu})
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    program = str(Path(args.program).resolve()) if Path(args.program).is_file() else args.program
    defaults = {"identity": 0, "energy": 1000000, "delta": 100000, "fir3": 100000}
    threshold = args.threshold if args.threshold is not None else defaults.get(program, 0)
    output.mkdir(parents=True)
    version_records = {}
    for label, binary in (("current", args.binary), ("baseline", args.baseline_binary)):
        value = subprocess.run([str(binary.resolve()), "--version"], capture_output=True, text=True, check=True).stdout.strip()
        version_records[label] = {"path": str(binary.resolve()), "sha256": sha256(binary), "version_output": value}
    if version_records["baseline"]["version_output"] != "suffice 0.3.0":
        raise RuntimeError("baseline must identify itself as Suffice 0.3.0; inspect the supplied source/binary")
    config = compiler_config(args.compiler)
    initial_inputs = {"raw_sha256": sha256(args.raw), "packed_sha256": sha256(args.packed),
                      "program": program, "program_sha256": sha256(program) if Path(program).is_file() else None}
    harness_sha256 = sha256(__file__)
    library, support = ensure_support(config, args.cache)
    definitions = list(VARIANTS)
    declarations = "\n".join(f'void register_{v["key"]}(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);' for v in definitions)
    (output / "benchmark_interface.hpp").write_text(INTERFACE + declarations + "\n", encoding="utf-8")
    variants, objects = [], []
    for variant in definitions:
        key = variant["key"]
        header = output / f"kernel_{key}.hpp"
        metadata = output / f"emission_{key}.json"
        binary = args.baseline_binary if variant["baseline"] else args.binary
        command = [str(binary.resolve()), "emit-cpp", "--input", str(args.packed.resolve()),
                   "--program", program, "--namespace", variant["namespace"], "--output", str(header),
                   "--json", str(metadata), *variant["flags"]]
        if args.threshold is not None or program in defaults:
            command += ["--threshold", str(threshold)]
        emission_ms = execute(command, output / f"emission_{key}.log", cwd=ROOT)
        emitted = json.loads(metadata.read_text(encoding="utf-8"))
        if not emitted.get("encoded_emitted"):
            raise RuntimeError("This focused comparison requires complete encoded lowering; unsupported program: " + program)
        source = output / f"wrapper_{key}.cpp"
        source.write_text(wrapper_source(variant), encoding="utf-8")
        obj = output / (f"wrapper_{key}.obj" if config["msvc"] else f"wrapper_{key}.o")
        compilation = compile_object(config, source, obj, output, output / f"compile_{key}.log")
        objects.append(obj)
        variants.append({**variant, "header": header.name, "header_sha256": sha256(header),
                         "emission_command": command, "emission_process_wall_ms": emission_ms,
                         "emission": emitted, "compilation": compilation})
    registrations = "\n".join(f'        register_{v["key"]}(candidates,aos.span(),soa.span(),rows,channels,packed);' for v in definitions)
    source = output / "driver.cpp"
    source.write_text(DRIVER.replace("@REGISTER_VARIANTS@", registrations), encoding="utf-8")
    obj = output / ("driver.obj" if config["msvc"] else "driver.o")
    driver_compilation = compile_object(config, source, obj, output, output / "compile_driver.log")
    objects.append(obj)
    executable = output / ("comparison.exe" if config["msvc"] else "comparison")
    if config["msvc"]:
        link = [config["executable"], "/nologo", *map(str, objects), str(library), "/Fe:" + str(executable)]
    else:
        link = [config["executable"], *config["flags"], *map(str, objects), str(library), "-o", str(executable)]
    link_ms = execute(link, output / "link.log", cwd=output)
    command = [str(executable), str(args.raw.resolve()), str(args.packed.resolve()), program,
               str(threshold), str(args.repeat), str(output / "measurements.json")]
    execution_ms = execute(command, output / "execution.log", cwd=ROOT)
    measurements = json.loads((output / "measurements.json").read_text(encoding="utf-8"))
    final_inputs = {"raw_sha256": sha256(args.raw), "packed_sha256": sha256(args.packed),
                    "program": program, "program_sha256": sha256(program) if Path(program).is_file() else None}
    if final_inputs != initial_inputs or sha256(__file__) != harness_sha256:
        raise RuntimeError("Input or benchmark script changed during the comparison")
    for label, binary in (("current", args.binary), ("baseline", args.baseline_binary)):
        if sha256(binary) != version_records[label]["sha256"]:
            raise RuntimeError("A generating executable changed during the comparison")
    if compiler_config(args.compiler) != config:
        raise RuntimeError("Compiler identity changed during the comparison")
    for relative, expected in support["identity"]["inputs"].items():
        if sha256(ROOT / relative) != expected:
            raise RuntimeError("Core source changed during the comparison: " + relative)
    for variant in variants:
        if sha256(output / variant["header"]) != variant["header_sha256"]:
            raise RuntimeError("Generated header changed during the comparison")
        if sha256(output / f'wrapper_{variant["key"]}.cpp') != variant["compilation"]["source_sha256"]:
            raise RuntimeError("Generated wrapper changed during the comparison")
    result = {
        "schema_version": 4, "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": {"platform": platform.platform(), "python": platform.python_version(),
                        "available_affinity_before": available, "benchmark_affinity": affinity},
        "binaries": version_records, "compiler": config,
        "inputs": initial_inputs, "harness_source_sha256": harness_sha256,
        "variants": variants, "shared_support_library": support, "driver_compilation": driver_compilation,
        "link": {"argv": link, "wall_ms": link_ms, "binary_sha256": sha256(executable)},
        "execution": {"argv": command, "process_wall_ms": execution_ms}, "measurements": measurements,
        "scope": [
            "All fresh candidate timings, including the frozen 0.3 kernel and current variants, occur in one process.",
            "Generated wrappers use separate translation units with no requested link-time optimization; compiler/linker code-layout effects remain possible.",
            "The scalar original IR supplies the complete oracle. Every fresh timed complete mask and count is verified after its timer.",
            "AoS and SoA raw controls use separate 64-byte-aligned copies of identical values. Copy and transpose costs are charged separately; per-column starts need not be 64-byte aligned for arbitrary row counts.",
            "Encoded inputs retain all bits. Data representation, execution algorithms and physical placement differ; no external-engine superiority is established.",
            "Output buffers are identically prefilled outside every timing interval. These are warm execution comparisons, not cold or end-to-end application latency.",
            "Prefix diagnostics are untimed scalar bound evaluations for eligible 256-row batches; counts are not instrumented hardware traffic.",
            "The three fixed prefix depths are all reported; a post-hoc fastest depth is not a deployed selector or an unseen-data guarantee.",
            "AOT emission, per-wrapper compilation, driver compilation, linking, and reusable support-library builds are real separately reported costs.",
            "Generated kernels are not installed in the automatic selector. The portable encoded-frontier12 core control is distinct.",
        ],
    }
    write_json(output / "result.json", result)
    print(json.dumps({"output": str(output), "exact": measurements["exact"],
                      "candidates": len(measurements["candidates"]), "rounds": measurements["repeat"],
                      "fresh_mask_checks": measurements["fresh_mask_checks"],
                      "range_mask_checks": measurements["range_mask_checks"],
                      "wrapper_compile_ms": sum(v["compilation"]["wall_ms"] for v in variants)}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
