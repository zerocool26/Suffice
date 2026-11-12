#!/usr/bin/env python3
"""Fixed 0.7 physical-scheduling experiment and actual bounded portfolio.

This reuses the retained full-mask/range/oracle driver. All header emission and
compilation precede native execution. A fixed bounded portfolio calibrates its
four eligible kernels before fresh comparison rounds; report-only envelopes do
not install a winner. Calibration and fresh rounds use the same immutable input.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

import benchmark_v0_5 as prior
from codegen_support import ROOT, compiler_config, ensure_support, execute, sha256

DEPTH = 12
PROOF_BUDGETS = {"encoded_proof_node_budget":65536,"encoded_proof_step_budget":1000000,
                 "encoded_proof_attempt_budget":256}
CALIBRATION = {"cycles":3,"minimum_cycles":2,"warmups_per_candidate":1,
               "minimum_improvement_basis_points":200,"max_row_evaluations_per_input_row":52,
               "max_elapsed_nanoseconds":250000000,"order_seed":0x53554646504F5254,
               "expected_remaining_calls":"actual_fresh_round_count"}
PORTFOLIO_LABEL = "v07-portfolio"
PORTFOLIO_NAMES = ("residual_guarded_pressure","care_guarded_pressure",
                   "residual_recompute_bounds_first","care_recompute_bounds_first")
PORTFOLIO_ELIGIBLE = ("v07-residual-pressure-guarded","v07-care-pressure-guarded",
                       "v07-residual-bounds-first-recompute","v07-care-bounds-first-recompute")
VARIANTS = (
    *({"key":"frozen_"+semantic+"_guarded", "namespace":"bench_frozen06_"+semantic, "baseline":True,
       "flags":["--encoded-strategy",strategy,"--encoded-prefix-bits","12","--encoded-schedule","pressure",
                "--encoded-carry-placement","guarded"],
       "raw":False,"encoded":True,"label":"v06-"+semantic+"-pressure-guarded","prefix":12,
       "strategy":strategy,"schedule":"pressure","placement":"guarded"}
      for semantic,strategy in (("residual","residual"),("care","guarded-residual"))),
    {"key":"eager", "namespace":"bench_eager07", "baseline":False,
     "flags":["--encoded-strategy","eager","--encoded-schedule","pressure"],
     "raw":True,"encoded":True,"label":"v07-eager","strategy":"eager","schedule":"pressure","placement":"eager"},
    {"key":"soa", "namespace":"bench_soa07", "baseline":False,
     "flags":["--raw-layout","planar","--encoded-strategy","eager","--encoded-schedule","pressure"],
     "raw":True,"encoded":False,"label":"raw-soa","strategy":"eager","schedule":"pressure","placement":"eager"},
    {"key":"frontier", "namespace":"bench_frontier07", "baseline":False,
     "flags":["--encoded-strategy","frontier","--encoded-prefix-bits","12","--encoded-schedule","pressure"],
     "raw":False,"encoded":True,"label":"v07-frontier","prefix":12,"strategy":"frontier","schedule":"pressure","placement":"eager"},
    *({"key":semantic+"_"+schedule.replace("-","_")+"_"+placement,
       "namespace":"bench07_"+semantic+"_"+schedule.replace("-","_")+"_"+placement,"baseline":False,
       "flags":["--encoded-strategy",strategy,"--encoded-prefix-bits","12","--encoded-schedule",schedule,
                "--encoded-carry-placement",placement],
       "raw":False,"encoded":True,"label":"v07-"+semantic+"-"+schedule+"-"+placement,"prefix":12,
       "strategy":strategy,"schedule":schedule,"placement":placement}
      for semantic,strategy in (("residual","residual"),("care","guarded-residual"))
      for placement in ("guarded","recompute") for schedule in ("pressure","bounds-first")),
)
RAW = ("raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd")
GENERATED = tuple(v["label"] for v in VARIANTS if v["encoded"])
PREFIX = tuple(v["label"] for v in VARIANTS if "prefix" in v)
CANDIDATES = (*GENERATED,*RAW,"portable-wordcode",PORTFOLIO_LABEL)
INTERFACE = prior.INTERFACE.replace(
    "    virtual bool uses_simd() const noexcept=0;",
    "    virtual bool uses_simd() const noexcept=0;\n"
    "    virtual std::string calibrate(std::span<const std::uint64_t>,std::span<std::uint64_t>,std::uint64_t) { return {}; }")


def header_metadata(path):
    """Retain generated constexpr metadata without executing generated code."""
    text=Path(path).read_text(encoding="utf-8");values={}
    for name,literal in re.findall(r'inline constexpr [^;\n]*?\b(encoded_[A-Za-z0-9_]+)\s*=\s*([^;\n]+);',text):
        literal=literal.strip()
        if literal in {"true","false"}:value=literal=="true"
        elif re.fullmatch(r'\d+(?:ULL|UL|U|LL|L)?',literal):value=int(re.sub(r'[UL]+$','',literal))
        elif literal.startswith('"'):
            try:value=json.loads(literal)
            except json.JSONDecodeError:continue
        else:continue
        values[name]=value
    return values


def default_baseline():
    return ROOT/"baseline/v0_6/build"/("suffice.exe" if os.name=="nt" else "suffice")


def source_identity(root):
    paths = sorted((root/"src").glob("*.cpp")) + sorted((root/"include").rglob("*.hpp"))
    if not paths: raise RuntimeError("missing frozen 0.6 source tree")
    return {p.relative_to(root).as_posix():sha256(p) for p in paths}


def frozen_release_identity():
    path=ROOT/"baseline/releases/Suffice_0_6_0.zip"
    checksum=sha256(path)
    if checksum!="64a1c21395f1850b876ff59b4eac02469c093310d9c0fd6593a78606f5378d17":
        raise RuntimeError("frozen full 0.6 release archive changed")
    return {"path":path.relative_to(ROOT).as_posix(),"sha256":checksum,"bytes":path.stat().st_size}


def driver_source(declarations,registrations,functional_pilot=False):
    source=prior.driver_source(declarations,registrations)
    for line in (
        '        add_core<suffice::DenseExecutor>(candidates,"dense","interleaved",raw,plan,custom?std::string{}:std::string(argv[3]),true,true);',
        '        add_core<suffice::VectorExecutor>(candidates,"raw-vector","interleaved",raw,plan);'):
        source=prior.replace_once(source,line,"")
    begin=source.index('        if(frontier.complete) {\n            add_core<suffice::FrontierExecutor>')
    end=source.index('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));',begin)
    end+=len('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));')
    source=source[:begin]+source[end:]
    source=prior.replace_once(source,'"v05-portable-wordcode"','"portable-wordcode"')
    source=prior.replace_once(source,'schema_version\\\":5','schema_version\\\":7')
    source=prior.replace_once(source,'0x5355464649434535ULL','0x5355464649434537ULL')
    source=prior.replace_once(source,'std::setprecision(12)','std::setprecision(std::numeric_limits<double>::max_digits10)')
    source=prior.replace_once(source,'"schema_version\\\":7,\\\"rows\\\":',
        '"schema_version\\\":7,\\\"measurement_purpose\\\":\\\"'+
        ('functional_pipeline_pilot' if functional_pilot else 'fixed_performance_experiment')+'\\\",\\\"rows\\\":')
    repetition=r'''        const auto minimum=std::stoul(argv[5]);
        if(minimum<1 || minimum>1000)throw std::invalid_argument("minimum repetition count must be 1..1000");
        const auto cycle=suffice::balanced_timing_cycle_rows(candidates.size());
        const auto repeat=static_cast<unsigned>(((minimum+cycle-1)/cycle)*cycle);'''
    source=prior.replace_once(source,repetition,"")
    source=prior.replace_once(source,'        phase=Clock::now();\n        std::uint64_t range_checks=0;',repetition+r'''
        // Calibration gets the original arithmetic-IR mask and an independent
        // scratch span. Its samples never enter fresh comparison samples.
        std::vector<std::uint64_t> calibration_storage(expected.size()+2,UINT64_MAX);
        auto calibration_scratch=std::span<std::uint64_t>(calibration_storage).subspan(1,expected.size());
        std::string portfolio_report;
        phase=Clock::now();
        for(auto& candidate:candidates) {
            const auto report=candidate.runner->calibrate(expected,calibration_scratch,repeat);
            if(!report.empty()) {
                if(!portfolio_report.empty())throw std::runtime_error("multiple portfolio selectors");
                portfolio_report=report;
            }
        }
        const auto portfolio_calibration_wall_ms=ms(phase);
        if(portfolio_report.empty() || calibration_storage.front()!=UINT64_MAX || calibration_storage.back()!=UINT64_MAX)
            throw std::runtime_error("missing portfolio calibration or scratch guard changed");
        phase=Clock::now();
        std::uint64_t range_checks=0;''')
    source=prior.replace_once(source,'              <<",\\\"warmup_ms\\\":"<<warmup_ms<<",\\\"timing_order_seed\\\":"<<timing_seed',r'''              <<",\"portfolio_calibration_wall_ms\":"<<portfolio_calibration_wall_ms
              <<",\"portfolio_reference_mask_checksum\":"<<suffice::mask_checksum(expected)
              <<",\"portfolio_reference_accepted\":"<<truth.accepted
              <<",\"portfolio_calibration\":"<<portfolio_report
              <<",\"post_calibration_samples_are_fresh\":true,\"calibration_and_measurement_input_same\":true"
              <<",\"warmup_ms\":"<<warmup_ms<<",\"timing_order_seed\":"<<timing_seed''')
    return source


def portfolio_wrapper_source():
    """Serialize the actual bounded selector, never install a reporting winner."""
    source=r'''
#include "benchmark_interface.hpp"
#include "suffice/storage.hpp"
#include "suffice/portfolio.hpp"
#include "kernel_portfolio.hpp"
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace {
class Portfolio final:public BenchRunner {
    bench_portfolio07::PreparedEncodedPortfolio<suffice::PackedDataset> kernel_;
public:
    explicit Portfolio(const suffice::PackedDataset& data):kernel_(data,true,true){}
    std::uint64_t run(std::span<std::uint64_t> mask) override {return kernel_.run(mask);}
    std::uint64_t run_range(std::uint64_t first,std::uint64_t count,std::span<std::uint64_t> mask) override {
        return kernel_.run_range(first,count,mask);
    }
    bool uses_simd() const noexcept override {return kernel_.uses_simd();}
    std::string calibrate(std::span<const std::uint64_t> expected,std::span<std::uint64_t> scratch,
                          std::uint64_t remaining) override {
        suffice::PortfolioCalibrationOptions options;
        options.cycles=@CYCLES@;options.minimum_cycles=@MINIMUM_CYCLES@;
        options.warmups_per_candidate=@WARMUPS@;
        options.minimum_improvement_basis_points=@MARGIN@;
        if(kernel_.rows()>UINT64_MAX/@ROW_MULTIPLIER@)throw std::overflow_error("calibration row budget overflow");
        options.max_row_evaluations=kernel_.rows()*@ROW_MULTIPLIER@;
        options.max_elapsed_nanoseconds=@TIME_BUDGET@ULL;
        options.expected_remaining_calls=remaining;options.order_seed=@ORDER_SEED@ULL;
        const auto report=kernel_.calibrate(0,kernel_.rows(),expected,scratch,options);
        std::ostringstream out;out<<std::setprecision(std::numeric_limits<double>::max_digits10);
        out<<"{\"candidate_count\":"<<report.candidate_count<<",\"incumbent_index\":"<<report.incumbent_index
           <<",\"proposed_index\":"<<report.proposed_index<<",\"selected_index\":"<<report.selected_index
           <<",\"selected_name\":\""<<kernel_.selected_name()<<"\",\"changed\":"<<(report.changed?"true":"false")
           <<",\"reason\":\""<<suffice::portfolio_reason_name(report.reason)
           <<"\",\"stop_reason\":\""<<suffice::portfolio_stop_reason_name(report.stop_reason)
           <<"\",\"completed_cycles\":"<<report.completed_cycles<<",\"total_calls\":"<<report.total_calls
           <<",\"row_evaluations\":"<<report.row_evaluations<<",\"elapsed_nanoseconds\":"<<report.elapsed_nanoseconds
           <<",\"estimated_break_even_calls\":"<<report.estimated_break_even_calls
           <<",\"break_even_saturated\":"<<(report.break_even_saturated?"true":"false")
           <<",\"incumbent_median_nanoseconds\":"<<report.incumbent_median_nanoseconds
           <<",\"proposed_median_nanoseconds\":"<<report.proposed_median_nanoseconds
           <<",\"estimated_saving_nanoseconds\":"<<report.estimated_saving_nanoseconds
           <<",\"view_storage_bytes\":"<<kernel_.view_storage_bytes()
           <<",\"first\":0,\"count\":"<<kernel_.rows()<<",\"options\":{\"cycles\":"<<options.cycles
           <<",\"minimum_cycles\":"<<options.minimum_cycles<<",\"warmups_per_candidate\":"<<options.warmups_per_candidate
           <<",\"minimum_improvement_basis_points\":"<<options.minimum_improvement_basis_points
           <<",\"max_row_evaluations\":"<<options.max_row_evaluations
           <<",\"max_elapsed_nanoseconds\":"<<options.max_elapsed_nanoseconds
           <<",\"expected_remaining_calls\":"<<options.expected_remaining_calls<<",\"order_seed\":"<<options.order_seed
           <<"},\"candidate_names\":[\"residual_guarded_pressure\",\"care_guarded_pressure\","
             "\"residual_recompute_bounds_first\",\"care_recompute_bounds_first\"],\"candidates\":[";
        for(std::size_t i=0;i<report.candidate_count;++i) {
            const auto& c=report.candidates[i];if(i)out<<',';
            out<<"{\"index\":"<<i<<",\"calls\":"<<c.calls<<",\"warmup_calls\":"<<c.warmup_calls
               <<",\"timed_samples\":"<<c.timed_samples<<",\"row_evaluations\":"<<c.row_evaluations
               <<",\"timed_nanoseconds\":"<<c.timed_nanoseconds<<",\"warmup_nanoseconds\":"<<c.warmup_nanoseconds
               <<",\"cycle_samples\":[";
            for(unsigned cycle=0;cycle<report.completed_cycles;++cycle){if(cycle)out<<',';out<<c.cycle_samples[cycle];}
            out<<"],\"cycle_median_nanoseconds\":[";
            for(unsigned cycle=0;cycle<report.completed_cycles;++cycle){if(cycle)out<<',';out<<c.cycle_median_nanoseconds[cycle];}
            out<<"],\"sample_nanoseconds\":[";
            for(unsigned cycle=0;cycle<report.completed_cycles;++cycle) {
                if(cycle)out<<',';out<<'[';
                for(unsigned j=0;j<c.cycle_samples[cycle];++j){if(j)out<<',';out<<c.sample_nanoseconds[cycle][j];}
                out<<']';
            }
            out<<"]}";
        }
        out<<"],\"cycles\":[";
        for(unsigned cycle=0;cycle<report.completed_cycles;++cycle) {
            const auto& c=report.cycles[cycle];if(cycle)out<<',';
            out<<"{\"round_count\":"<<c.round_count<<",\"orders\":[";
            for(unsigned round=0;round<c.round_count;++round) {
                if(round)out<<',';out<<'[';
                for(std::size_t i=0;i<report.candidate_count;++i){if(i)out<<',';out<<unsigned(c.orders[round][i]);}
                out<<']';
            }
            out<<"],\"median_nanoseconds\":[";
            for(std::size_t i=0;i<report.candidate_count;++i){if(i)out<<',';out<<c.median_nanoseconds[i];}
            out<<"]}";
        }
        out<<"]}";return out.str();
    }
};
}
void register_portfolio(std::vector<Candidate>& candidates,Native,Native,std::uint64_t,std::uint32_t,
                        const suffice::PackedDataset& packed) {
    const auto begin=std::chrono::steady_clock::now();
    auto runner=std::make_unique<Portfolio>(packed);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    candidates.push_back({"v07-portfolio","packed","generated-bounded-portfolio",std::move(runner),{},elapsed,{}});
}
'''
    substitutions={"CYCLES":CALIBRATION["cycles"],"MINIMUM_CYCLES":CALIBRATION["minimum_cycles"],
                   "WARMUPS":CALIBRATION["warmups_per_candidate"],"MARGIN":CALIBRATION["minimum_improvement_basis_points"],
                   "ROW_MULTIPLIER":CALIBRATION["max_row_evaluations_per_input_row"],
                   "TIME_BUDGET":CALIBRATION["max_elapsed_nanoseconds"],"ORDER_SEED":CALIBRATION["order_seed"]}
    for key,value in substitutions.items():source=source.replace("@"+key+"@",str(value))
    return source


def verify_native_artifacts(result,directory):
    if sha256(directory/"benchmark_interface.hpp")!=result["interface_sha256"]:
        raise RuntimeError("prepared native interface changed")
    for v in (*result["variants"],result["portfolio"]):
        for path,expected in ((directory/v["header"],v["header_sha256"]),
                             (directory/f'emission_{v["key"]}.json',v["emission_metadata_sha256"]),
                             (directory/f'wrapper_{v["key"]}.cpp',v["compilation"]["source_sha256"]),
                             (directory/(f'wrapper_{v["key"]}'+(".obj" if result["compiler"]["msvc"] else ".o")),v["compilation"]["object_sha256"])):
            if sha256(path)!=expected:raise RuntimeError("prepared artifact changed: "+str(path))
    for name,key in (("driver.cpp","source_sha256"),("driver.obj" if result["compiler"]["msvc"] else "driver.o","object_sha256")):
        if sha256(directory/name)!=result["driver_compilation"][key]:raise RuntimeError("prepared native driver changed")
    executable=directory/("comparison.exe" if result["compiler"]["msvc"] else "comparison")
    if sha256(executable)!=result["link"]["binary_sha256"]:raise RuntimeError("prepared comparison executable changed")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary",type=Path,default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    p.add_argument("--baseline-binary",type=Path,default=default_baseline())
    p.add_argument("--raw",type=Path,required=True);p.add_argument("--packed",type=Path,required=True)
    p.add_argument("--program",required=True);p.add_argument("--threshold",type=int)
    p.add_argument("--output",type=Path,required=True);p.add_argument("--repeat",type=int,default=36)
    p.add_argument("--compiler");p.add_argument("--cache",type=Path)
    p.add_argument("--pin-cpu",default="auto");p.add_argument("--prepare-only",action="store_true")
    p.add_argument("--functional-pilot",action="store_true",help="Label every output as functional pipeline evidence only")
    args=p.parse_args()
    if not 1<=args.repeat<=1000:p.error("repeat must be 1..1000")
    for path in (args.binary,args.baseline_binary,args.raw,args.packed):
        if not path.is_file():p.error("required file missing: "+str(path))
    output=args.output.resolve()
    if output.exists():p.error("use a fresh output directory")
    available=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    if args.pin_cpu!="none" and available:
        cpu=available[0] if args.pin_cpu=="auto" else int(args.pin_cpu)
        if cpu not in available:p.error("CPU outside permitted affinity")
        os.sched_setaffinity(0,{cpu})
    affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    program=str(Path(args.program).resolve()) if Path(args.program).is_file() else args.program
    defaults={"identity":0,"energy":1000000,"delta":100000,"fir3":100000}
    threshold=args.threshold if args.threshold is not None else defaults.get(program,0)
    versions={}
    for label,binary in (("current",args.binary),("baseline",args.baseline_binary)):
        version=subprocess.run([str(binary.resolve()),"--version"],capture_output=True,text=True,check=True).stdout.strip()
        versions[label]={"path":str(binary.resolve()),"sha256":sha256(binary),"version_output":version}
    if versions["baseline"]["version_output"]!="suffice 0.6.0" or versions["current"]["version_output"]!="suffice 0.7.0":
        raise RuntimeError("requires frozen 0.6 and current 0.7 executable identities")
    config=compiler_config(args.compiler)
    initial={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
             "program_sha256":sha256(program) if Path(program).is_file() else None}
    harness={str(Path(x).relative_to(ROOT)):sha256(x) for x in (__file__,prior.__file__,prior.previous.__file__,ROOT/"scripts/codegen_support.py")}
    frozen_sources=source_identity(ROOT/"baseline/v0_6")
    frozen_release=frozen_release_identity()
    output.mkdir(parents=True)
    library,support=ensure_support(config,args.cache)
    (output/"benchmark_interface.hpp").write_text(INTERFACE,encoding="utf-8")
    variants,objects=[],[]
    for variant in VARIANTS:
        key=variant["key"];header=output/f"kernel_{key}.hpp";metadata=output/f"emission_{key}.json"
        binary=args.baseline_binary if variant["baseline"] else args.binary
        command=[str(binary.resolve()),"emit-cpp","--input",str(args.packed.resolve()),"--program",program,
                 "--namespace",variant["namespace"],"--output",str(header),"--json",str(metadata),*variant["flags"]]
        if args.threshold is not None or program in defaults:command += ["--threshold",str(threshold)]
        emission_ms=execute(command,output/f"emission_{key}.log",cwd=ROOT)
        emitted=json.loads(metadata.read_text(encoding="utf-8"))
        if not emitted.get("encoded_emitted"):raise RuntimeError("focused experiment requires complete encoded lowering")
        if emitted["encoded_strategy"]!=variant["strategy"]:raise RuntimeError("emitted strategy differs from request")
        if emitted["encoded_schedule"]!=variant["schedule"]:raise RuntimeError("emitted schedule differs from request")
        if "residual" in variant["strategy"] and not emitted.get("encoded_residual_emitted"):
            raise RuntimeError("requested residual silently fell back")
        if variant["strategy"]=="guarded-residual" and not emitted.get("encoded_guarded_residual_emitted"):
            raise RuntimeError("requested guarded residual silently fell back")
        if variant["strategy"]=="guarded-residual" and any(emitted.get(k)!=v for k,v in PROOF_BUDGETS.items()):
            raise RuntimeError("predeclared default proof budgets changed")
        if emitted.get("encoded_carry_placement")!=variant["placement"]:
            raise RuntimeError("emitted carry placement differs from request")
        if variant["placement"]=="recompute" and not emitted.get("encoded_recompute_carry_emitted"):
            raise RuntimeError("requested recomputation silently fell back")
        source=output/f"wrapper_{key}.cpp";source.write_text(prior.wrapper_source(variant),encoding="utf-8")
        obj=output/(f"wrapper_{key}"+(".obj" if config["msvc"] else ".o"))
        compiled=prior.compile_object(config,source,obj,output,output/f"compile_{key}.log")
        objects.append(obj)
        variants.append({**variant,"header":header.name,"header_sha256":sha256(header),"header_metadata":header_metadata(header),
                         "emission_metadata_sha256":sha256(metadata),"emission_command":command,
                         "emission_process_wall_ms":emission_ms,"emission":emitted,"compilation":compiled})
    header=output/"kernel_portfolio.hpp";metadata=output/"emission_portfolio.json"
    command=[str(args.binary.resolve()),"emit-portfolio","--input",str(args.packed.resolve()),"--program",program,
             "--namespace","bench_portfolio07","--output",str(header),"--json",str(metadata),"--encoded-prefix-bits","12"]
    if args.threshold is not None or program in defaults:command += ["--threshold",str(threshold)]
    emission_ms=execute(command,output/"emission_portfolio.log",cwd=ROOT)
    emitted=json.loads(metadata.read_text(encoding="utf-8"))
    if not emitted.get("portfolio_emitted") or emitted.get("candidate_count")!=4 or emitted.get("fallback_index")!=0:
        raise RuntimeError("portfolio fixed inventory or fallback changed")
    if emitted.get("runtime_calibration_performed") is not False:
        raise RuntimeError("code generation unexpectedly performed runtime calibration")
    if tuple(v["name"] for v in emitted["variants"])!=PORTFOLIO_NAMES:
        raise RuntimeError("portfolio candidate names/order changed")
    source=output/"wrapper_portfolio.cpp";source.write_text(portfolio_wrapper_source(),encoding="utf-8")
    obj=output/("wrapper_portfolio.obj" if config["msvc"] else "wrapper_portfolio.o")
    compiled=prior.compile_object(config,source,obj,output,output/"compile_portfolio.log");objects.append(obj)
    portfolio={"key":"portfolio","namespace":"bench_portfolio07","header":header.name,
               "header_sha256":sha256(header),"emission_metadata_sha256":sha256(metadata),"emission_command":command,
               "emission_process_wall_ms":emission_ms,"emission":emitted,"compilation":compiled}
    keys=[v["key"] for v in VARIANTS]+["portfolio"]
    declarations="\n".join(f'void register_{key}(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);' for key in keys)
    registrations="\n".join(f'        register_{key}(candidates,aos.span(),soa.span(),rows,channels,packed);' for key in keys)
    source=output/"driver.cpp";source.write_text(driver_source(declarations,registrations,args.functional_pilot),encoding="utf-8")
    obj=output/("driver.obj" if config["msvc"] else "driver.o")
    compiled=prior.compile_object(config,source,obj,output,output/"compile_driver.log");objects.append(obj)
    executable=output/("comparison.exe" if config["msvc"] else "comparison")
    link=([config["executable"],"/nologo",*map(str,objects),str(library),"/Fe:"+str(executable)] if config["msvc"]
          else [config["executable"],*config["flags"],*map(str,objects),str(library),"-o",str(executable)])
    link_ms=execute(link,output/"link.log",cwd=output)
    command=[str(executable),str(args.raw.resolve()),str(args.packed.resolve()),program,str(threshold),str(args.repeat),str(output/"measurements.json")]
    execution_ms=measurements=None
    if not args.prepare_only:
        execution_ms=execute(command,output/"execution.log",cwd=ROOT)
        measurements=json.loads((output/"measurements.json").read_text(encoding="utf-8"))
        if {c["backend"] for c in measurements["candidates"]}!=set(CANDIDATES):raise RuntimeError("candidate inventory changed")
    if initial!={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
                 "program_sha256":sha256(program) if Path(program).is_file() else None}:raise RuntimeError("benchmark input changed")
    for relative,expected in harness.items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("benchmark harness changed")
    for label,binary in (("current",args.binary),("baseline",args.baseline_binary)):
        if sha256(binary)!=versions[label]["sha256"]:raise RuntimeError("generating binary changed")
    if compiler_config(args.compiler)!=config:raise RuntimeError("compiler identity changed")
    for relative,expected in support["identity"]["inputs"].items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("core source changed: "+relative)
    if source_identity(ROOT/"baseline/v0_6")!=frozen_sources:raise RuntimeError("frozen source changed")
    if frozen_release_identity()!=frozen_release:raise RuntimeError("frozen full release changed")
    result={"schema_version":7,"status":"prepared" if args.prepare_only else "completed",
            "measurement_purpose":"functional_pipeline_pilot" if args.functional_pilot else "fixed_performance_experiment",
            "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "environment":{"platform":platform.platform(),"python":platform.python_version(),"available_affinity_before":available,"benchmark_affinity":affinity},
            "binaries":versions,"compiler":config,"inputs":initial,"harness_sources":harness,"fixed_prefix_bits":DEPTH,
            "fixed_proof_budgets":PROOF_BUDGETS,"fixed_calibration":CALIBRATION,
            "candidate_inventory":list(CANDIDATES),"portfolio_eligible_controls":list(PORTFOLIO_ELIGIBLE),
            "variants":variants,"portfolio":portfolio,"shared_support_library":support,"frozen_sources":frozen_sources,
            "frozen_release":frozen_release,
            "driver_compilation":compiled,"interface_sha256":sha256(output/"benchmark_interface.hpp"),
            "link":{"argv":link,"wall_ms":link_ms,"binary_sha256":sha256(executable)},
            "execution":{"argv":command,"process_wall_ms":execution_ms},"measurements":measurements,
            "scope":[
                "Eighteen candidates coexist in one process; separate wrapper translation units; no requested LTO.",
                "Frozen controls are emitted by the preserved 0.6 CLI and compiled by the same current host compiler. They use the shared packed reader interface, not a separate full 0.6 process. The complete original 0.6 release archive is retained and hashed.",
                "Every fresh complete mask and each arbitrary range mask is checked against the original scalar arithmetic IR outside timing.",
                "Current all-lane/care, Pressure/BoundsFirst, and guarded/recompute choices form a two-by-two-by-two experiment. The controls distinguish scheduling from cold-path rematerialization.",
                "Raw AoS/SoA scalar/SIMD and current eager/wordcode/frontier remain explicit controls. Copy/transpose/validation/compile costs are separate.",
                "Generated prefix diagnostics reconstruct 64/256-row branch geometry outside timing. They do not measure hardware traffic or stack high-water marks.",
                "Compiler/generator wall times are observed; isolated compiler or proof-engine peak memory is not measured.",
                "Depth12, proof budgets, candidate inventory, calibration budget, margin and expected reuse horizon are fixed before execution. The actual generated portfolio calibrates its four fixed candidates and is measured in new rounds after selection.",
                "Calibration observes the same immutable input as fresh measurements. This is not held-out generalization. Report-only best-candidate envelopes never replace the deployed portfolio decision.",
                "Calibration has 52 input passes of row work, a 250ms soft elapsed budget checked between complete cycles, and three requested four-round cycles. Its expected remaining calls equal the actual fresh repetition count.",
                "These warm synthetic results do not establish Windows, application-wide, arbitrary-AI, external-engine, or hardware-optimal gains."
            ]}
    prior.previous.write_json(output/"result.json",result)
    print(json.dumps({"output":str(output),"status":result["status"],"candidates":len(CANDIDATES),
                      "exact":measurements["exact"] if measurements else None},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
