#!/usr/bin/env python3
"""Compile an emitted kernel and compare it with exact prepared core backends."""

import argparse
import datetime
import json
from pathlib import Path
import platform
import sys

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256


DRIVER = r'''
#include "generated_kernel.hpp"
#include "suffice/ir.hpp"
#include "suffice/storage.hpp"
#include "suffice/runtime.hpp"
#include "suffice/predicate.hpp"
#include "suffice/vector.hpp"
#include "suffice/wordcode.hpp"
#include "suffice/benchmark_order.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
struct Candidate { std::string name; std::function<std::uint64_t(std::span<std::uint64_t>)> run; std::vector<double> samples; };

int main(int argc, char** argv) {
    try {
        if (argc != 7) throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD REPEAT OUTPUT_JSON");
        const auto began = Clock::now();
        suffice::PackedDataset packed(argv[2]);
        suffice::RawDataset raw(argv[1], packed.info().channels);
        packed.verify_payload();
        if(raw.info().rows != packed.info().rows) throw std::invalid_argument("raw/packed row mismatch");
        std::vector<std::int32_t> block(packed.info().block_rows);
        for(std::uint64_t b=0;b<packed.block_count();++b) for(std::uint32_t c=0;c<packed.info().channels;++c) {
            packed.decode_block(b,c,block);
            for(std::uint32_t j=0;j<packed.block_length(b);++j)
                if(block[j]!=raw.value(b*packed.info().block_rows+j,c)) throw std::runtime_error("raw/packed mismatch");
        }
        const auto validation_ms = ms(began);
        auto phase = Clock::now();
        std::ifstream source(argv[3]);
        const bool custom = source.good();
        const std::string text = custom ? std::string(std::istreambuf_iterator<char>(source),{}) : std::string{};
        auto program = custom ? suffice::parse_program(text) : suffice::builtin_program(argv[3], std::stoll(argv[4]));
        std::vector<suffice::Bounds> ranges;
        for(const auto r:packed.info().ranges) ranges.push_back({r.min,r.max});
        auto plan=program.compile(ranges);
        auto predicate=suffice::compile_predicate(plan);
        auto wordcode=suffice::compile_wordcode(predicate);
        const auto plan_ms=ms(phase);
        const auto rows=raw.info().rows;
        std::vector<std::uint64_t> expected(suffice::mask_words(rows)),output(expected.size());
        phase=Clock::now();
        suffice::DenseExecutor reference(raw,plan,{},false,false);
        const auto expected_stats=reference.run(expected);
        const auto oracle_ms=ms(phase);
        phase=Clock::now();
        const auto native_count=raw.bytes().size()/sizeof(std::int32_t);
        const auto native_delete=[](std::int32_t* pointer){::operator delete[](pointer,std::align_val_t{64});};
        std::unique_ptr<std::int32_t,decltype(native_delete)> native_storage(
            static_cast<std::int32_t*>(::operator new[](raw.bytes().size(),std::align_val_t{64})),native_delete);
        std::span<std::int32_t> native(native_storage.get(),native_count);
        raw.read_rows(0,rows,native);
        const auto native_copy_ms=ms(phase);
        phase=Clock::now();
        benchmark_generated::PreparedKernel generated_scalar(native,rows,raw.info().channels,false);
        benchmark_generated::PreparedKernel generated_simd(native,rows,raw.info().channels,true);
        std::unique_ptr<benchmark_generated::PreparedEncodedKernel<suffice::PackedDataset>> generated_encoded;
        std::unique_ptr<benchmark_generated::PreparedEncodedKernel<suffice::PackedDataset>> generated_encoded_scalar;
        if constexpr(benchmark_generated::encoded_emitted) {
            generated_encoded=std::make_unique<benchmark_generated::PreparedEncodedKernel<suffice::PackedDataset>>(packed,true);
            generated_encoded_scalar=std::make_unique<benchmark_generated::PreparedEncodedKernel<suffice::PackedDataset>>(packed,true,false);
        }
        const auto generated_prepare_ms=ms(phase);
        phase=Clock::now();
        suffice::DenseExecutor dense(raw,plan,custom ? "" : argv[3],true,true);
        suffice::VectorExecutor raw_vector(raw,plan),packed_vector(packed,plan);
        std::unique_ptr<suffice::DensePredicateExecutor> dp;
        std::unique_ptr<suffice::EncodedPredicateExecutor> ep;
        std::unique_ptr<suffice::WordcodeExecutor> wc;
        if(predicate.complete) { dp=std::make_unique<suffice::DensePredicateExecutor>(raw,predicate); ep=std::make_unique<suffice::EncodedPredicateExecutor>(packed,predicate); }
        if(wordcode.complete) wc=std::make_unique<suffice::WordcodeExecutor>(packed,wordcode);
        const auto fixed_prepare_ms=ms(phase);
        std::vector<Candidate> candidates;
        candidates.push_back({"generated-scalar",[&](auto mask){return generated_scalar.run(mask);},{}});
        candidates.push_back({"generated-simd",[&](auto mask){return generated_simd.run(mask);},{}});
        if(generated_encoded) candidates.push_back({"generated-encoded",[&](auto mask){return generated_encoded->run(mask);},{}});
        if(generated_encoded_scalar) candidates.push_back({"generated-encoded-scalar",[&](auto mask){return generated_encoded_scalar->run(mask);},{}});
        candidates.push_back({"dense",[&](auto mask){return dense.run(mask).accepted;},{}});
        candidates.push_back({"raw-vector",[&](auto mask){return raw_vector.run(mask).accepted;},{}});
        candidates.push_back({"packed-vector",[&](auto mask){return packed_vector.run(mask).accepted;},{}});
        if(dp) candidates.push_back({"dense-predicate",[&](auto mask){return dp->run(mask).accepted;},{}});
        if(ep) candidates.push_back({"encoded-predicate",[&](auto mask){return ep->run(mask).accepted;},{}});
        if(wc) candidates.push_back({"encoded-wordcode",[&](auto mask){return wc->run(mask).accepted;},{}});
        auto verify=[&](std::uint64_t accepted) { if(accepted!=expected_stats.accepted || output!=expected) throw std::runtime_error("generated comparison mismatch"); };
        phase=Clock::now();
        for(auto& candidate:candidates) verify(candidate.run(output));
        const auto warmup_ms=ms(phase);
        const auto repeat=std::stoul(argv[5]);
        if(repeat<1 || repeat>1000) throw std::invalid_argument("invalid repetition count");
        std::uint64_t checksum=0,checks=0;
        const auto orders=suffice::balanced_timing_orders(candidates.size(),static_cast<unsigned>(repeat),0x5355464649434533ULL);
        for(unsigned round=0;round<repeat;++round) {
            for(std::size_t slot=0;slot<candidates.size();++slot) {
                auto& candidate=candidates[orders[round][slot]];
                phase=Clock::now(); const auto accepted=candidate.run(output); const auto elapsed=ms(phase);
                verify(accepted); candidate.samples.push_back(elapsed); ++checks;
                checksum^=suffice::mask_checksum(output)+round+slot;
            }
        }
        std::ofstream result(argv[6]); if(!result) throw std::runtime_error("cannot open result JSON");
        result << std::setprecision(12) << "{\n\"schema_version\":1,\"rows\":"<<rows<<",\"channels\":"<<raw.info().channels
               <<",\"repeat\":"<<repeat<<",\"predicate_lowered\":"<<(benchmark_generated::predicate_lowered?"true":"false")
               <<",\"generated_simd_active\":"<<(generated_simd.uses_simd()?"true":"false")
               <<",\"encoded_emitted\":"<<(benchmark_generated::encoded_emitted?"true":"false")
               <<",\"encoded_avx2_emitted\":"<<(benchmark_generated::encoded_avx2_emitted?"true":"false")
               <<",\"generated_encoded_simd_active\":"<<(generated_encoded && generated_encoded->uses_simd()?"true":"false")
               <<",\"encoded_simd_rows\":"<<benchmark_generated::encoded_simd_rows
               <<",\"wordcode_nodes\":"<<benchmark_generated::wordcode_nodes
               <<",\"encoded_plane_count\":"<<benchmark_generated::encoded_plane_count
               <<",\"source_nodes\":"<<benchmark_generated::source_nodes<<",\"predicate_nodes\":"<<benchmark_generated::predicate_nodes
               <<",\"simd_lanes\":"<<benchmark_generated::simd_lanes<<",\"accepted\":"<<expected_stats.accepted
               <<",\"exact\":true,\"fresh_mask_checks\":"<<checks<<",\"mask_checksum\":"<<suffice::mask_checksum(expected)
               <<",\"checksum_accumulator\":"<<checksum<<",\"validation_ms\":"<<validation_ms<<",\"plan_ms\":"<<plan_ms
               <<",\"oracle_ms\":"<<oracle_ms<<",\"native_copy_ms\":"<<native_copy_ms
               <<",\"native_copy_bytes\":"<<native.size()*sizeof(std::int32_t)
               <<",\"native_copy_alignment_bytes\":64"
               <<",\"generated_prepare_ms\":"<<generated_prepare_ms<<",\"fixed_prepare_ms\":"<<fixed_prepare_ms
               <<",\"warmup_ms\":"<<warmup_ms<<",\"timing_order_seed\":6004782945859224883"
               <<",\"timing_order_protocol\":\"randomized Williams cycles\""
               <<",\"timing_cycle_rows\":"<<suffice::balanced_timing_cycle_rows(candidates.size())<<",\"timing_order\":[";
        for(std::size_t r=0;r<orders.size();++r) {
            if(r)result<<',';
            result<<'[';
            for(std::size_t j=0;j<orders[r].size();++j){if(j)result<<',';result<<'\"'<<candidates[orders[r][j]].name<<'\"';}
            result<<']';
        }
        result<<"],\"candidates\":[\n";
        bool comma=false;
        for(const auto& c:candidates) {
            auto sorted=c.samples;std::sort(sorted.begin(),sorted.end());
            const auto median=sorted.size()%2?sorted[sorted.size()/2]:(sorted[sorted.size()/2-1]+sorted[sorted.size()/2])/2;
            if(comma)result<<",\n";
            comma=true;
            result<<"{\"backend\":\""<<c.name<<"\",\"median_ms\":"<<median<<",\"min_ms\":"<<sorted.front()<<",\"max_ms\":"<<sorted.back()<<",\"samples_ms\":[";
            for(std::size_t i=0;i<c.samples.size();++i){if(i)result<<',';result<<c.samples[i];}
            result<<"]}";
        }
        result << "\n]}\n";
        std::cout<<"PASS "<<checks<<" fresh full-mask checks; generated SIMD "<<(generated_simd.uses_simd()?"active":"inactive")<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/suffice")
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--packed", required=True, type=Path)
    parser.add_argument("--program", required=True)
    parser.add_argument("--threshold", type=int)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=7)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    args = parser.parse_args()
    if not 1 <= args.repeat <= 1000:
        parser.error("repeat must be in 1..1000")
    output = args.output.resolve()
    if output.exists():
        parser.error("output directory already exists; use a fresh path")
    output.mkdir(parents=True)
    program = str(Path(args.program).resolve()) if Path(args.program).is_file() else args.program
    config = compiler_config(args.compiler)
    library, support = ensure_support(config, args.cache)
    header = output / "generated_kernel.hpp"
    command = [args.binary.resolve(), "emit-cpp", "--input", args.packed.resolve(),
               "--program", program, "--namespace", "benchmark_generated", "--output", header]
    defaults = {"identity": 0, "energy": 1000000, "delta": 100000, "fir3": 100000}
    threshold = args.threshold if args.threshold is not None else defaults.get(program, 0)
    if args.threshold is not None or program in defaults:
        command += ["--threshold", str(threshold)]
    emission_ms = execute(command, output / "emission.log", cwd=ROOT)
    driver = output / "driver.cpp"
    driver.write_text(DRIVER, encoding="utf-8")
    executable = output / ("generated_benchmark.exe" if config["msvc"] else "generated_benchmark")
    compilation = compile_program(config, driver, executable, library, output / "compile.log", include=output)
    execution = [executable, args.raw.resolve(), args.packed.resolve(), program,
                 str(threshold), str(args.repeat), output / "measurements.json"]
    execution_ms = execute(execution, output / "execution.log", cwd=ROOT)
    measurements = json.loads((output / "measurements.json").read_text(encoding="utf-8"))
    result = {
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": platform.platform(), "compiler": config,
        "cli_binary_sha256": sha256(args.binary.resolve()), "generated_header_sha256": sha256(header),
        "raw_sha256": sha256(args.raw), "packed_sha256": sha256(args.packed),
        "program": program, "program_sha256": sha256(program) if Path(program).is_file() else None,
        "emission_command": list(map(str, command)), "emission_process_wall_ms": emission_ms,
        "shared_support_library": support, "kernel_compilation": compilation,
        "execution_command": list(map(str, execution)), "execution_process_wall_ms": execution_ms,
        "measurements": measurements,
        "scope": [
            "Generated scalar and AVX2 kernels are fused from this program, not a generic DAG interpreter.",
            "The reference executes the original scalar IR; every fresh complete mask is checked after its timer.",
            "Native kernels use a host-native int32 copy of the same raw interleaved values; its allocation/copy and range validation are outside execution timers and reported.",
            "All other controls use the same original mapped files. Warm timing cannot prove cold or application performance.",
            "AOT compilation and shared support-library compilation are separate real costs; cache reuse is explicit.",
            "No generated code is installed as an automatic-selector candidate by this diagnostic driver.",
        ],
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "exact": measurements["exact"],
                      "fresh_mask_checks": measurements["fresh_mask_checks"],
                      "kernel_compile_ms": compilation["wall_ms"],
                      "support_cache_hit": support["cache_hit"]}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
