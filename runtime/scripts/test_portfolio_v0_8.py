#!/usr/bin/env python3
"""Compile real combined portfolios and check exact, bounded prepared execution.

All speed observations are used only by calibration. This correctness suite
asserts no fastest candidate and publishes no timing-based speedup claim.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256
from test_codegen import FIXTURES, TEST_BODY


BODY = r'''
#include <type_traits>
struct CountedDataset {
    const suffice::PackedDataset& source;
    mutable unsigned verifications{};
    mutable std::uint64_t plane_calls{}, grouped_calls{};
    explicit CountedDataset(const suffice::PackedDataset& data):source(data){}
    const suffice::DatasetInfo& info() const noexcept {return source.info();}
    std::uint64_t block_count() const noexcept {return source.block_count();}
    std::uint32_t block_length(std::uint64_t block) const {return source.block_length(block);}
    std::span<const std::uint8_t> plane_view(std::uint64_t block,std::uint32_t channel,
                                           std::uint32_t bit,std::uint32_t count) const {
        ++plane_calls;if(count>1)++grouped_calls;
        return source.plane_view(block,channel,bit,count);
    }
    void verify_payload() const {++verifications;source.verify_payload();}
};
std::uint64_t mask_checks=0,allocation_checks=0,calibrations=0,payload_checks=0;
std::filesystem::path fixture_directory;
std::uint64_t file_number=0;

template<template<class> class Portfolio>
void exercise_portfolio(const char* text,std::uint32_t channels,
                        std::int64_t minimum,std::int64_t maximum) {
    using Prepared=Portfolio<CountedDataset>;
    static_assert(!std::is_copy_constructible_v<Prepared>);
    static_assert(!std::is_move_constructible_v<Prepared>);
    static_assert(!std::is_copy_assignable_v<Prepared>);
    static_assert(!std::is_move_assignable_v<Prepared>);
    const auto plan=suffice::parse_program(text).compile(std::vector<suffice::Bounds>(channels,{minimum,maximum}));
    for(const std::uint64_t rows:{0u,1u,65u,257u,1031u}) {
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows*channels));
        std::uint64_t state=0xacef93756;
        const auto width=std::uint64_t(maximum-minimum)+1;
        for(std::size_t i=0;i<values.size();++i) {
            state^=state<<13;state^=state>>7;state^=state<<17;
            auto value=minimum+std::int64_t(state%width);
            if(i%7==0)value=minimum;else if(i%7==1)value=maximum;
            else if(i%7==2)value=std::clamp<std::int64_t>(0,minimum,maximum);
            values[i]=static_cast<std::int32_t>(value);
        }
        const auto base=fixture_directory/("portfolio_"+std::to_string(file_number++));
        const auto raw=base.string()+".i32";
        {std::ofstream file(raw,std::ios::binary);require(bool(file),"cannot create portfolio fixture");
         for(auto value:values) {const auto bits=std::bit_cast<std::uint32_t>(value);
             for(unsigned byte=0;byte<4;++byte)file.put(static_cast<char>(bits>>(8*byte)));}}
        for(const std::uint32_t block_rows:{32u,256u}) {
            const auto packed=base.string()+"_"+std::to_string(block_rows)+".sfp";
            (void)suffice::pack_file(raw,packed,channels,block_rows);
            suffice::PackedDataset mapped(packed);
            CountedDataset data(mapped);
            Prepared portfolio(data);
            require(data.verifications==1,"portfolio payload was not verified exactly once");++payload_checks;
            Prepared scalar(data,true,false);
            const Prepared const_portfolio(data,true,true);
            require(data.verifications==1&&!scalar.uses_simd(),"already-verified or scalar-only preparation ignored");
            require(portfolio.rows()==rows&&portfolio.candidate_count()==4&&portfolio.selected_index()==0,
                    "portfolio fallback or dataset metadata invalid");
            require(portfolio.selected_name()==portfolio.candidate_name(0),"portfolio names disagree");
            std::vector<std::uint64_t> starts{0,rows/2,rows};
            for(const auto first:{1u,3u,31u,63u,255u,256u})if(first<=rows)starts.push_back(first);
            auto check=[&](std::uint64_t first,std::uint64_t count) {
                const auto expected=reference(plan,values,rows,channels,first,count);
                std::uint64_t accepted_count=0;for(const auto word:expected)accepted_count+=std::popcount(word);
                std::vector<std::uint64_t> storage(expected.size()+2,0xead49bff33687716ULL);
                auto output=std::span(storage).subspan(1,expected.size());
                const std::array<const Prepared*,3> prepared_owners{{&portfolio,&scalar,&const_portfolio}};
                for(const auto* candidate:prepared_owners) {
                    allocations.store(0);track.store(true);
                    const auto accepted=first==0&&count==rows ? candidate->run(output) :
                        candidate->run_range(first,count,output);
                    track.store(false);require(allocations.load()==0,"portfolio valid execution allocated");++allocation_checks;
                    require(accepted==accepted_count&&std::equal(output.begin(),output.end(),expected.begin()),
                            "portfolio differs from original arithmetic IR");++mask_checks;
                    require(storage.front()==0xead49bff33687716ULL&&storage.back()==0xead49bff33687716ULL,
                            "portfolio overwrote output guard words");
                }
            };
            for(const auto first:starts) {
                check(first,0);check(first,rows-first);
                for(const auto count:{1u,31u,64u,65u,255u,256u})if(count<=rows-first)check(first,count);
            }
            auto calibrate=[&](std::uint64_t first,std::uint64_t count) {
                const auto expected=reference(plan,values,rows,channels,first,count);
                std::vector<std::uint64_t> storage(expected.size()+2,0xb0d63d35703f4be2ULL);
                auto scratch=std::span(storage).subspan(1,expected.size());
                suffice::PortfolioCalibrationOptions options;
                options.max_row_evaluations=52*count;
                options.max_elapsed_nanoseconds=std::numeric_limits<std::uint64_t>::max();
                options.expected_remaining_calls=100;
                allocations.store(0);track.store(true);
                const auto report=portfolio.calibrate(first,count,expected,scratch,options);
                track.store(false);require(allocations.load()==0,"portfolio valid calibration allocated");++allocation_checks;
                if(count) {
                    require(report.completed_cycles==3&&report.total_calls==52&&report.row_evaluations==52*count,
                            "generated portfolio complete-cycle accounting mismatch");
                    for(unsigned i=0;i<4;++i)require(report.candidates[i].calls==13&&report.candidates[i].timed_samples==12,
                                                   "generated candidate was not checked in every cycle");
                } else require(report.total_calls==0&&report.reason==suffice::PortfolioReason::EmptyRange,
                               "empty generated calibration spent work");
                require(report.selected_index==portfolio.selected_index(),"calibration commit metadata differs");
                require(storage.front()==0xb0d63d35703f4be2ULL&&storage.back()==0xb0d63d35703f4be2ULL,
                        "calibration overwrote scratch guard words");
                const auto incumbent=portfolio.selected_index();
                rejects([&]{(void)portfolio.calibrate(first,count,expected,std::span<std::uint64_t>(
                    const_cast<std::uint64_t*>(expected.data()),expected.size()),options);});
                require(portfolio.selected_index()==incumbent,"invalid calibration changed selected kernel");
                ++calibrations;
            };
            if(rows) {
                calibrate(0,rows);check(0,rows);
                if(rows>65){calibrate(3,std::min<std::uint64_t>(129,rows-3));check(3,std::min<std::uint64_t>(129,rows-3));}
            } else {
                suffice::PortfolioCalibrationOptions options;
                require(portfolio.calibrate(0,0,{},{},options).reason==suffice::PortfolioReason::EmptyRange,
                        "empty dataset calibration changed state");
            }
            std::vector<std::uint64_t> output(static_cast<std::size_t>(rows/64+(rows%64!=0)));
            rejects([&]{(void)portfolio.run_range(rows+1,0,{});});
            rejects([&]{(void)portfolio.run_range(0,std::numeric_limits<std::uint64_t>::max(),output);});
            rejects([&]{(void)portfolio.candidate_name(4);});
            output.emplace_back(0);rejects([&]{(void)portfolio.run(output);});
        }
    }
}
'''


def specifications(smoke=False):
    result = [
        ("magnitude", FIXTURES[2], True, None),
        ("and_scalar", ("and_scalar", 2, -2147483648, 2147483647,
                         "input x 0 0\ninput y 1 0\nmin both x y\noutput both > 1000\n"), False, None),
        ("proof_fallback", FIXTURES[2], True, 0),
        ("constant", FIXTURES[10], True, None),
    ]
    return result[:2] if smoke else result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--binding", choices=("per-plane","grouped","both"), default="both")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    config = compiler_config(args.compiler, args.sanitize)
    suffix = "_sanitized" if args.sanitize else ""
    work = ROOT / "build" / ("portfolio_v0_8" + suffix)
    work.mkdir(parents=True, exist_ok=True)
    library, support = ensure_support(config)
    modes = ("per-plane", "grouped") if args.binding == "both" else (args.binding,)
    specs = [(name + "_" + binding, fixture, avx2, proof_budget, binding)
             for name, fixture, avx2, proof_budget in specifications(args.smoke) for binding in modes]
    generator = ['#include "suffice/portfolio_codegen.hpp"', '#include <filesystem>',
                 '#include <fstream>', '#include <iostream>', '#include <vector>',
                 'int main(int argc,char** argv){try{if(argc!=2)return 2;std::filesystem::path directory=argv[1];']
    for index, (_, fixture, avx2, proof_budget, binding) in enumerate(specs):
        _, channels, minimum, maximum, text = fixture
        generator += ["{", f"const auto plan=suffice::parse_program({json.dumps(text)}).compile("
                      f"std::vector<suffice::Bounds>({channels},{{{minimum}LL,{maximum}LL}}));",
                      f'suffice::CodegenOptions options;options.namespace_name="portfolio_case{index}";',
                      f'options.enable_avx2={str(avx2).lower()};',
                      f'options.encoded_plane_binding=suffice::EncodedPlaneBinding::{"Grouped" if binding == "grouped" else "PerPlane"};']
        if proof_budget is not None:
            generator.append(f"options.guarded_residual_options.proof_step_budget={proof_budget};")
        generator += ["const auto generated=suffice::emit_cpp_portfolio(plan,options);",
                      f'std::ofstream file(directory/"portfolio_{index}.hpp");file<<generated.header;',
                      'if(!file)throw std::runtime_error("cannot write combined generated header");', "}"]
    generator += ['return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\\n";return 1;}}']
    source = work / "emit_portfolios.cpp"
    source.write_text("\n".join(generator) + "\n", encoding="utf-8")
    executable = work / ("emit_portfolios.exe" if config["msvc"] else "emit_portfolios")
    generator_build = compile_program(config, source, executable, library, work / "emit_compile.log")
    environment = os.environ.copy()
    if args.sanitize:
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
    generation_ms = execute([executable, work], work / "emit_run.log", env=environment)
    headers = [work / f"portfolio_{i}.hpp" for i in range(len(specs))]
    prelude = TEST_BODY.split("std::uint64_t mask_checks=0,allocation_checks=0;", 1)[0]
    if prelude == TEST_BODY:
        raise RuntimeError("original-IR fixture prelude integration point changed")
    harness = "\n".join(f'#include "{header.name}"' for header in headers) + prelude + BODY
    harness += '\nint main(int argc,char** argv){try{if(argc!=2)return 2;fixture_directory=argv[1];std::filesystem::create_directories(fixture_directory);\n'
    for index, (_, fixture, _, _, _) in enumerate(specs):
        _, channels, minimum, maximum, text = fixture
        harness += (f"exercise_portfolio<portfolio_case{index}::PreparedEncodedPortfolio>("
                    f"{json.dumps(text)},{channels},{minimum}LL,{maximum}LL);\n")
    harness += r'''std::cout<<"{\"mask_checks\":"<<mask_checks<<",\"allocation_checks\":"<<allocation_checks
        <<",\"calibrations\":"<<calibrations<<",\"payload_checks\":"<<payload_checks<<"}\n";
        return 0;}catch(const std::exception& e){track.store(false);std::cerr<<e.what()<<"\n";return 1;}}
'''
    harness_source = work / "exercise_portfolios.cpp"
    harness_source.write_text(harness, encoding="utf-8")
    harness_binary = work / ("exercise_portfolios.exe" if config["msvc"] else "exercise_portfolios")
    harness_build = compile_program(config, harness_source, harness_binary, library, work / "exercise_compile.log", include=work)
    fixture_directory = work / ("fixtures_" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%f"))
    validation_ms = execute([harness_binary, fixture_directory], work / "exercise_run.log", env=environment)
    summary = json.loads((work / "exercise_run.log").read_text(encoding="utf-8"))
    report = {
        "version": "0.8.0", "suite": "generated_portfolio", "status": "PASS",
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "sanitize": args.sanitize, "smoke": args.smoke, "compiler": config,
        "support": support, "generator_build": generator_build, "harness_build": harness_build,
        "generation_ms": generation_ms, "validation_ms": validation_ms,
        "portfolios": [{"name": spec[0], "header": str(header.relative_to(ROOT)),
                        "header_bytes": header.stat().st_size, "header_sha256": sha256(header)}
                       for spec, header in zip(specs, headers)],
        "checks": summary,
        "binding_modes": list(modes), "test_source_sha256": sha256(Path(__file__)),
        "scope": "Both per-plane and grouped bindings; Original-arithmetic-IR masks; full/ranged prepared calls; all four candidates checked in complete calibration cycles; payload verified once; no speed thresholds.",
    }
    output = args.output or ROOT / "results" / ("portfolio_v0_8_correctness" + suffix + ".json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "report": str(output), **summary}))


if __name__ == "__main__":
    main()
