#!/usr/bin/env python3
"""Controlled 0.8 grouped-binding benchmark, with a separate binding-plus-sink experiment.

The six encoded scans hold Pressure/guarded lowering fixed: frozen 0.7,
current single-plane, and current grouped binding for each residual contract.
Four raw controls remain. Generation/compilation precede all timing.
"""
from __future__ import annotations
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

import benchmark_v0_5 as prior
from benchmark_v0_7 import header_metadata
from codegen_support import ROOT, compiler_config, ensure_support, execute, sha256

DEPTH = 12
BINDING_PASSES = 32
PROOF_BUDGETS = {"encoded_proof_node_budget":65536,"encoded_proof_step_budget":1000000,
                 "encoded_proof_attempt_budget":256}
VARIANTS = (
    *({"key":semantic+"_"+binding.replace("-","_"), "namespace":"bench08_"+semantic+"_"+binding.replace("-","_"),
       "baseline":binding=="frozen", "binding":"per-plane" if binding=="frozen" else binding,
       "flags":["--encoded-strategy",strategy,"--encoded-prefix-bits","12","--encoded-schedule","pressure",
                "--encoded-carry-placement","guarded"] +
               ([] if binding=="frozen" else ["--encoded-plane-binding",binding]),
       "raw":False,"encoded":True,"label":("v07-" if binding=="frozen" else "v08-")+semantic+"-"+
               ("per-plane" if binding=="frozen" else binding), "prefix":12,
       "strategy":strategy,"schedule":"pressure","placement":"guarded"}
      for semantic,strategy in (("residual","residual"),("care","guarded-residual"))
      for binding in ("frozen","per-plane","grouped")),
    *({"key":layout,"namespace":"bench08_raw_"+layout,"baseline":False,"binding":"per-plane",
       "flags":["--encoded-strategy","eager","--encoded-schedule","pressure"]+
               (["--raw-layout","planar"] if layout=="soa" else []),
       "raw":True,"encoded":False,"label":"raw-"+layout,"strategy":"eager","schedule":"pressure","placement":"eager"}
      for layout in ("aos","soa")),
)
RAW = ("raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd")
GENERATED = tuple(v["label"] for v in VARIANTS if v["encoded"])
PREFIX = GENERATED
CANDIDATES = (*GENERATED,*RAW)
BINDING_VARIANTS = ("residual_per_plane","residual_grouped")
BINDING_NAMES = ("binding-per-plane","binding-grouped")
INTERFACE = prior.INTERFACE.replace(
    "    virtual bool uses_simd() const noexcept=0;",
    "    virtual bool uses_simd() const noexcept=0;\n"
    "    virtual std::string binding_probe(std::span<const std::uint64_t>) { return \"null\"; }")
INTERFACE = INTERFACE.replace("    PrefixDiagnostic diagnostic;\n",
    "    PrefixDiagnostic diagnostic;\n    std::vector<std::uint64_t> samples_ns{};\n    std::string binding_diagnostic{\"null\"};\n")
INTERFACE += r'''
namespace allocation_probe {void start() noexcept;std::uint64_t stop() noexcept;}
struct BindingRunner {
    virtual ~BindingRunner()=default;
    virtual std::uint64_t run(unsigned passes)=0;
    virtual std::string inspect()=0;
};
struct BindingCandidate {
    std::string name;
    std::unique_ptr<BindingRunner> runner;
    std::vector<std::uint64_t> samples_ns;
    std::string diagnostic;
};
extern "C" void consume_plane_views(const std::span<const std::uint8_t>*,std::size_t) noexcept;
extern "C" std::uint64_t plane_sink_value() noexcept;
'''

# The sink is a different translation unit with no LTO. Every block's original
# slot-ordered array escapes to a consumer that reads all pointer/size pairs.
# It reads no input payload. The cost is intentionally included, never subtracted.
SINK = r'''
#include "benchmark_interface.hpp"
#if defined(_MSC_VER)
#define BIND_NOINLINE __declspec(noinline)
#else
#define BIND_NOINLINE __attribute__((noinline))
#endif
namespace { volatile std::uint64_t observed_sink{}; }
extern "C" BIND_NOINLINE void consume_plane_views(
    const std::span<const std::uint8_t>* views,std::size_t count) noexcept {
    std::uint64_t value=0x9e3779b97f4a7c15ULL;
    for(std::size_t i=0;i<count;++i) {
        value^=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(views[i].data()))+
               static_cast<std::uint64_t>(views[i].size())+i;
        value=(value<<7)|(value>>57);
    }
    observed_sink=observed_sink+(value|1ULL);
}
extern "C" BIND_NOINLINE std::uint64_t plane_sink_value() noexcept { return observed_sink; }
'''

COUNTED_DATASET = r'''
class CountedDataset {
    const suffice::PackedDataset& data_;
public:
    mutable std::uint64_t calls{},requested_planes{},returned_bytes{},direct_length_calls{};
    explicit CountedDataset(const suffice::PackedDataset& data):data_(data){}
    const auto& info() const noexcept {return data_.info();}
    std::uint64_t block_count() const noexcept {return data_.block_count();}
    std::uint32_t block_length(std::uint64_t block) const {++direct_length_calls;return data_.block_length(block);}
    auto plane_view(std::uint64_t block,std::uint32_t channel,std::uint32_t first,std::uint32_t count) const {
        auto view=data_.plane_view(block,channel,first,count);
        ++calls;requested_planes+=count;returned_bytes+=view.size();return view;
    }
    void verify_payload() const {data_.verify_payload();}
};
'''

SCAN_PROBE = r'''
    std::string binding_probe(std::span<const std::uint64_t> expected) override {
        CountedDataset counted(data_);
        generated::PreparedEncodedKernel<CountedDataset> checked(counted,true,true);
        std::vector<std::uint64_t> mask(expected.size(),UINT64_MAX);
        counted.calls=counted.requested_planes=counted.returned_bytes=counted.direct_length_calls=0;
        allocation_probe::start();
        const auto accepted=checked.run(mask);
        const auto allocated=allocation_probe::stop();
        if(allocated || accepted!=suffice::mask_popcount(expected) ||
           !std::equal(mask.begin(),mask.end(),expected.begin(),expected.end()))
            throw std::runtime_error("counted binding scan differs or allocates");
        std::ostringstream out;
        out<<"{\"exact_mask\":true,\"allocations\":"<<allocated
           <<",\"observed_plane_view_calls\":"<<counted.calls
           <<",\"requested_planes\":"<<counted.requested_planes
           <<",\"returned_view_bytes\":"<<counted.returned_bytes
           <<",\"direct_block_length_calls\":"<<counted.direct_length_calls
           <<",\"blocks\":"<<data_.block_count()
           <<",\"plane_slots\":"<<generated::encoded_plane_count
           <<",\"expected_binding_calls_per_block\":"<<@EXPECTED_CALLS@<<"}";
        return out.str();
    }
'''

BINDER = r'''
namespace {
class Binder final:public BindingRunner {
    const suffice::PackedDataset& data_;
    generated::EncodedPlaneViews planes_{};
public:
    explicit Binder(const suffice::PackedDataset& data):data_(data){}
    std::uint64_t run(unsigned passes) override {
        std::uint64_t bound_rows=0;
        for(unsigned pass=0;pass<passes;++pass)for(std::uint64_t b=0;b<data_.block_count();++b) {
            bound_rows+=generated::bind_encoded_planes(data_,b,planes_);
            consume_plane_views(planes_.data(),planes_.size());
        }
        return bound_rows;
    }
    std::string inspect() override {
        CountedDataset counted(data_);
        generated::EncodedPlaneViews views{};
        std::uint64_t slots_checked=0,rows=0,minimum_plane_bytes=UINT64_MAX,maximum_plane_bytes=0;
        for(std::uint64_t b=0;b<data_.block_count();++b) {
            const auto length=generated::bind_encoded_planes(counted,b,views);
            if(length!=data_.block_length(b))throw std::runtime_error("binder returned wrong block length");
            rows+=length;
            const auto bytes=(std::uint64_t{length}+7)/8;
            minimum_plane_bytes=std::min(minimum_plane_bytes,bytes);maximum_plane_bytes=std::max(maximum_plane_bytes,bytes);
            for(std::size_t i=0;i<views.size();++i) {
                const auto p=generated::plane_contracts[i];
                const auto direct=data_.plane_view(b,p.channel,p.bit,1);
                if(views[i].data()!=direct.data() || views[i].size()!=direct.size() || views[i].size()!=bytes)
                    throw std::runtime_error("binder changed original slot identity/extent");
                ++slots_checked;
            }
        }
        allocation_probe::start();const auto bound=run(1);const auto allocated=allocation_probe::stop();
        if(allocated || bound!=data_.info().rows)throw std::runtime_error("binding-only call allocates or changes row extent");
        std::ostringstream out;
        out<<"{\"original_slot_identities_checked\":true,\"slots_checked\":"<<slots_checked
           <<",\"rows\":"<<rows<<",\"blocks\":"<<data_.block_count()
           <<",\"allocations\":"<<allocated<<",\"plane_slots\":"<<generated::encoded_plane_count
           <<",\"groups\":"<<generated::encoded_binding_groups
           <<",\"expected_binding_calls_per_block\":"<<generated::encoded_binding_calls_per_block
           <<",\"observed_plane_view_calls\":"<<counted.calls<<",\"requested_planes\":"<<counted.requested_planes
           <<",\"returned_view_bytes\":"<<counted.returned_bytes
           <<",\"direct_block_length_calls\":"<<counted.direct_length_calls
           <<",\"minimum_plane_bytes\":"<<(rows?minimum_plane_bytes:0)<<",\"maximum_plane_bytes\":"<<maximum_plane_bytes
           <<",\"plane_contracts\":[";
        for(std::size_t i=0;i<generated::plane_contracts.size();++i) {
            if(i)out<<',';
            const auto p=generated::plane_contracts[i];out<<'['<<p.channel<<','<<p.bit<<']';
        }
        out<<"]}";return out.str();
    }
};
}
void register_binding_@KEY@(std::vector<BindingCandidate>& candidates,const suffice::PackedDataset& data) {
    candidates.push_back({"@BINDING_NAME@",std::make_unique<Binder>(data),{},{}});
}
'''


def default_baseline():
    return ROOT/"baseline/v0_7/build"/("suffice.exe" if os.name=="nt" else "suffice")


def source_identity(root):
    paths=sorted((root/"src").glob("*.cpp"))+sorted((root/"include").rglob("*.hpp"))
    if not paths:raise RuntimeError("missing frozen0.7 sources")
    return {p.relative_to(root).as_posix():sha256(p) for p in paths}


def frozen_release_identity():
    path=ROOT/"baseline/releases/Suffice_0_7_0.zip"
    checksum=sha256(path)
    if checksum!="b78c0b787ff17c9c4defc21af00cdc65d4775ff7f5c32a30e994631bc7633d33":
        raise RuntimeError("frozen0.7 archive changed")
    return {"path":path.relative_to(ROOT).as_posix(),"sha256":checksum,"bytes":path.stat().st_size}


def cpu_identity():
    """Record the observed model, without inferring a model from capabilities."""
    data={"machine":platform.machine(),"platform_processor":platform.processor(),"logical_cpu_count":os.cpu_count()}
    proc=Path("/proc/cpuinfo")
    if proc.is_file():
        text=proc.read_text(errors="replace")
        record={}
        for line in text.split("\n\n",1)[0].splitlines():
            if ":" in line:
                key,value=line.split(":",1);record[key.strip()]=value.strip()
        data.update({"source":"/proc/cpuinfo first processor","model":record.get("model name") or record.get("Hardware"),
                     "vendor":record.get("vendor_id"),"flags":record.get("flags",record.get("Features","")).split()})
    elif os.name=="nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                data.update({"source":"Windows processor registry","model":winreg.QueryValueEx(key,"ProcessorNameString")[0].strip()})
        except OSError:
            data.update({"source":"platform.processor only","model":None})
    else:data.update({"source":"platform.processor only","model":None})
    return data


def encoded_bodies(path):
    """Hash exact encoded arithmetic bodies, excluding namespace-specific macro prefixes."""
    text=Path(path).read_text();out={}
    pattern=r"^[^\n]*\binline[^\n]*\b((?:evaluate|run)_encoded_[A-Za-z0-9_]+)\s*\("
    for match in re.finditer(pattern,text,re.M):
        start=text.index("{",match.end());level=0;end=None
        for pos in range(start,len(text)):
            if text[pos]=="{":level+=1
            elif text[pos]=="}":
                level-=1
                if not level:end=pos+1;break
        if end is None:raise RuntimeError("unterminated encoded body")
        name=match.group(1)
        if name in out:raise RuntimeError("duplicate encoded body: "+name)
        out[name]=hashlib.sha256(text[start:end].encode()).hexdigest()
    if not out:raise RuntimeError("no encoded arithmetic bodies")
    contracts=re.search(r"\bplane_contracts\{\{(.*?)\}\};",text,re.S)
    if not contracts:raise RuntimeError("missing plane contracts")
    return {"functions":out,"plane_contracts_sha256":hashlib.sha256(contracts.group(1).encode()).hexdigest()}


def wrapper_source(variant):
    source=prior.wrapper_source(variant)
    source=source.replace('#include <stdexcept>','#include <stdexcept>\n#include <sstream>\n#include <algorithm>\n#include "suffice/runtime.hpp"')
    source=prior.replace_once(source,'class Encoded final : public BenchRunner {',COUNTED_DATASET+'\nclass Encoded final : public BenchRunner {')
    if variant["encoded"]:
        expected="generated::encoded_plane_count" if variant["baseline"] else "generated::encoded_binding_calls_per_block"
        source=prior.replace_once(source,"    PrefixDiagnostic diagnose(",SCAN_PROBE.replace("@EXPECTED_CALLS@",expected)+"\n    PrefixDiagnostic diagnose(")
    if variant["key"] in BINDING_VARIANTS:
        source+=BINDER.replace("@KEY@",variant["key"]).replace("@BINDING_NAME@",BINDING_NAMES[BINDING_VARIANTS.index(variant["key"])])
    return source


def allocation_source():
    source=(ROOT/"tests/main.cpp").read_text().split("\nvoid test_ir();",1)[0]
    return source.replace('#include "allocation_probe.hpp"',"")


def driver_source(declarations,registrations,binding_declarations,binding_registrations,functional_pilot=False):
    source=prior.driver_source(declarations,registrations)
    for line in (
        '        add_core<suffice::DenseExecutor>(candidates,"dense","interleaved",raw,plan,custom?std::string{}:std::string(argv[3]),true,true);',
        '        add_core<suffice::VectorExecutor>(candidates,"raw-vector","interleaved",raw,plan);',
        '        if(wordcode.complete)add_core<suffice::WordcodeExecutor>(candidates,"v05-portable-wordcode","packed",packed,wordcode);'):
        source=prior.replace_once(source,line,"")
    begin=source.index('        if(frontier.complete) {\n            add_core<suffice::FrontierExecutor>')
    end=source.index('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));',begin)
    end+=len('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));')
    source=source[:begin]+source[end:]
    source=source.replace('        auto predicate=suffice::compile_predicate(plan);\n        auto wordcode=suffice::compile_wordcode(predicate);\n        auto frontier=suffice::compile_frontier(predicate,12);\n',"")
    source=prior.replace_once(source,'using Clock=std::chrono::steady_clock;',allocation_source()+"\n"+binding_declarations+r'''
using Clock=std::chrono::steady_clock;
static_assert(Clock::is_steady);
std::uint64_t elapsed_ns(Clock::time_point begin) {
    const auto value=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
    if(value<=0)throw std::runtime_error("nonpositive integer timer observation");
    return static_cast<std::uint64_t>(value);
}
''')
    source=prior.replace_once(source,'if(argc!=7)throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD MIN_REPEAT OUTPUT_JSON");',
        'if(argc!=8)throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD MIN_REPEAT OUTPUT_JSON BINDING_PASSES");')
    source=prior.replace_once(source,'        const auto channels=raw.info().channels;',r'''        const auto channels=raw.info().channels;
        const auto binding_passes=std::stoul(argv[7]);
        if(!binding_passes || binding_passes>1024 || rows>UINT64_MAX/binding_passes)
            throw std::invalid_argument("invalid binding pass count");
''')
    source=prior.replace_once(source,'        const auto generated_prepare_ms=ms(phase);',r'''        const auto generated_prepare_ms=ms(phase);
        std::vector<BindingCandidate> bindings;
'''+binding_registrations+r'''
        if(bindings.size()!=2)throw std::runtime_error("binding inventory differs");
''')
    source=prior.replace_once(source,'        for(auto& candidate:candidates)candidate.diagnostic=candidate.runner->diagnose(expected);',r'''        std::uint64_t allocation_checks=0;
        for(auto& candidate:candidates) {
            candidate.diagnostic=candidate.runner->diagnose(expected);
            candidate.binding_diagnostic=candidate.runner->binding_probe(expected);
            if(candidate.binding_diagnostic!="null")++allocation_checks;
            std::fill(output.begin(),output.end(),UINT64_MAX);
            allocation_probe::start();
            const auto accepted=candidate.runner->run(output);
            const auto allocated=allocation_probe::stop();
            verify(candidate,accepted);
            if(allocated)throw std::runtime_error("prepared scan allocated");
            ++allocation_checks;
        }
        for(auto& binding:bindings)binding.diagnostic=binding.runner->inspect();
        allocation_checks+=bindings.size();
''')
    source=prior.replace_once(source,'        for(auto& c:candidates)c.samples.reserve(repeat);',
        '        for(auto& c:candidates){c.samples.reserve(repeat);c.samples_ns.reserve(repeat);}')
    source=prior.replace_once(source,'0x5355464649434535ULL','0x5355464649434538ULL')
    source=prior.replace_once(source,'phase=Clock::now();const auto accepted=candidate.runner->run(output);const auto elapsed=ms(phase);',
        'phase=Clock::now();const auto accepted=candidate.runner->run(output);const auto ns=elapsed_ns(phase);const auto elapsed=double(ns)/1000000.0;')
    source=prior.replace_once(source,'            candidate.samples.push_back(elapsed);++checks;',
        '            candidate.samples.push_back(elapsed);candidate.samples_ns.push_back(ns);++checks;')
    source=prior.replace_once(source,'        std::ofstream result(argv[6]);',r'''        // This separate experiment contains binding plus the common observable
        // sink/loop cost. Never subtract it from a full-scan timing.
        constexpr std::uint64_t binding_seed=0x5355464642494e38ULL;
        const auto binding_orders=suffice::balanced_timing_orders(bindings.size(),repeat,binding_seed);
        for(auto& binding:bindings){binding.samples_ns.reserve(repeat);binding.runner->run(1);}
        for(unsigned round=0;round<repeat;++round)for(auto index:binding_orders[round]) {
            auto& binding=bindings[index];
            phase=Clock::now();const auto bound=binding.runner->run(static_cast<unsigned>(binding_passes));
            const auto elapsed=elapsed_ns(phase);
            if(bound!=rows*binding_passes)throw std::runtime_error("binding extent checksum differs");
            binding.samples_ns.push_back(elapsed);
        }
        std::ofstream result(argv[6]);''')
    source=prior.replace_once(source,'std::setprecision(12)','std::setprecision(std::numeric_limits<double>::max_digits10)')
    source=prior.replace_once(source,'schema_version\\\":5','schema_version\\\":8')
    source=prior.replace_once(source,'"schema_version\\\":8,\\\"rows\\\":',
        '"schema_version\\\":8,\\\"measurement_purpose\\\":\\\"'+('functional_pipeline_pilot' if functional_pilot else 'fixed_performance_experiment')+'\\\",\\\"rows\\\":')
    source=prior.replace_once(source,'              <<",\\\"range_mask_checks\\\":"<<range_checks<<",\\\"checksum_accumulator\\\":"<<checksum',
        '              <<",\\\"allocation_checks\\\":"<<allocation_checks<<",\\\"clock_is_steady\\\":true,\\\"clock_period_num\\\":"<<Clock::period::num<<",\\\"clock_period_den\\\":"<<Clock::period::den\n'
        '              <<",\\\"range_mask_checks\\\":"<<range_checks<<",\\\"checksum_accumulator\\\":"<<checksum')
    source=prior.replace_once(source,'            result<<"],\\\"prefix_diagnostic\\\":{\\\"available\\\":"',
        '            result<<"],\\\"samples_ns\\\":[";\n'
        "            for(std::size_t j=0;j<c.samples_ns.size();++j){if(j)result<<',';result<<c.samples_ns[j];}\n"
        '            result<<"],\\\"binding_probe\\\":"<<c.binding_diagnostic<<",\\\"prefix_diagnostic\\\":{\\\"available\\\":"')
    source=prior.replace_once(source,'        result<<"\\n]}\\n";',r'''        result<<"\n],\"binding_experiment\":{\"passes_per_sample\":"<<binding_passes
              <<",\"repeat\":"<<repeat<<",\"timing_order_seed\":"<<binding_seed
              <<",\"timing_cycle_rows\":2,\"complete_timing_cycles\":"<<repeat/2
              <<",\"timing_order\":[";
        for(std::size_t i=0;i<binding_orders.size();++i) {
            if(i)result<<',';result<<'[';
            for(std::size_t j=0;j<binding_orders[i].size();++j){if(j)result<<',';result<<'"'<<bindings[binding_orders[i][j]].name<<'"';}
            result<<']';
        }
        result<<"],\"candidates\":[";
        for(std::size_t i=0;i<bindings.size();++i) {
            const auto& binding=bindings[i];if(i)result<<',';
            result<<"{\"name\":\""<<binding.name<<"\",\"diagnostic\":"<<binding.diagnostic<<",\"samples_ns\":[";
            for(std::size_t j=0;j<binding.samples_ns.size();++j){if(j)result<<',';result<<binding.samples_ns[j];}
            result<<"]}";
        }
        result<<"],\"observable_sink\":"<<plane_sink_value()
              <<",\"scope\":\"Binding plus out-of-line pointer/size sink and loop overhead; no payload read or Boolean execution; not subtractable from full-scan timing.\"}}\n";''')
    source=source.replace('}catch(const std::exception& e){std::cerr<<e.what()', '}catch(const std::exception& e){allocation_probe::stop();std::cerr<<e.what()')
    return source


def verify_native_artifacts(result,directory):
    if sha256(directory/"benchmark_interface.hpp")!=result["interface_sha256"]:raise RuntimeError("native interface changed")
    suffix=".obj" if result["compiler"]["msvc"] else ".o"
    for v in result["variants"]:
        for path,digest in ((directory/v["header"],v["header_sha256"]),
                           (directory/f'emission_{v["key"]}.json',v["emission_metadata_sha256"]),
                           (directory/f'wrapper_{v["key"]}.cpp',v["compilation"]["source_sha256"]),
                           (directory/(f'wrapper_{v["key"]}'+suffix),v["compilation"]["object_sha256"])):
            if sha256(path)!=digest:raise RuntimeError("native artifact changed: "+str(path))
    for base,key in (("driver","driver_compilation"),("binding_sink","sink_compilation")):
        for name,digest in ((base+".cpp",result[key]["source_sha256"]),(base+suffix,result[key]["object_sha256"])):
            if sha256(directory/name)!=digest:raise RuntimeError("native source/object changed: "+name)
    executable=directory/("comparison.exe" if result["compiler"]["msvc"] else "comparison")
    if sha256(executable)!=result["link"]["binary_sha256"]:raise RuntimeError("comparison binary changed")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary",type=Path,default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    p.add_argument("--baseline-binary",type=Path,default=default_baseline())
    p.add_argument("--raw",type=Path,required=True);p.add_argument("--packed",type=Path,required=True)
    p.add_argument("--program",required=True);p.add_argument("--threshold",type=int)
    p.add_argument("--output",type=Path,required=True);p.add_argument("--repeat",type=int,default=40)
    p.add_argument("--compiler");p.add_argument("--cache",type=Path);p.add_argument("--pin-cpu",default="auto")
    p.add_argument("--prepare-only",action="store_true");p.add_argument("--functional-pilot",action="store_true")
    args=p.parse_args()
    if not 1<=args.repeat<=1000:p.error("repeat must be 1..1000")
    for path in (args.binary,args.baseline_binary,args.raw,args.packed):
        if not path.is_file():p.error("required file missing: "+str(path))
    output=args.output.resolve()
    if output.exists():p.error("use a fresh output directory")
    available=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    if args.pin_cpu not in {"auto","none"} and not available:
        p.error("explicit CPU pinning is unsupported on this platform; use auto/none or pin the outer native process")
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
    if versions["baseline"]["version_output"]!="suffice 0.7.0" or versions["current"]["version_output"]!="suffice 0.8.0":
        raise RuntimeError("requires frozen0.7 and current0.8 executables")
    config=compiler_config(args.compiler)
    initial={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
             "program_sha256":sha256(program) if Path(program).is_file() else None}
    harness_paths=(__file__,prior.__file__,prior.previous.__file__,ROOT/"scripts/codegen_support.py",
                   ROOT/"scripts/benchmark_v0_7.py",ROOT/"tests/main.cpp")
    harness={str(Path(x).relative_to(ROOT)):sha256(x) for x in harness_paths}
    frozen=source_identity(ROOT/"baseline/v0_7");release=frozen_release_identity()
    output.mkdir(parents=True)
    library,support=ensure_support(config,args.cache)
    (output/"benchmark_interface.hpp").write_text(INTERFACE)
    variants,objects=[],[]
    for variant in VARIANTS:
        key=variant["key"];header=output/f"kernel_{key}.hpp";metadata=output/f"emission_{key}.json"
        binary=args.baseline_binary if variant["baseline"] else args.binary
        command=[str(binary.resolve()),"emit-cpp","--input",str(args.packed.resolve()),"--program",program,
                 "--namespace",variant["namespace"],"--output",str(header),"--json",str(metadata),*variant["flags"]]
        if args.threshold is not None or program in defaults:command+=["--threshold",str(threshold)]
        emission_ms=execute(command,output/f"emission_{key}.log",cwd=ROOT)
        emitted=json.loads(metadata.read_text())
        for field in ("strategy","schedule","carry_placement"):
            wanted=variant["placement" if field=="carry_placement" else field]
            if emitted.get("encoded_"+field)!=wanted:raise RuntimeError("emitted physical contract differs: "+field)
        if not emitted.get("encoded_emitted"):raise RuntimeError("requires complete encoded lowering")
        if variant["encoded"] and not emitted.get("encoded_residual_emitted"):raise RuntimeError("residual fell back")
        if variant["strategy"]=="guarded-residual":
            if not emitted.get("encoded_guarded_residual_emitted") or any(emitted.get(k)!=v for k,v in PROOF_BUDGETS.items()):
                raise RuntimeError("care lowering/proof budget changed")
        if not variant["baseline"] and emitted.get("encoded_plane_binding")!=variant["binding"]:
            raise RuntimeError("requested binding not emitted")
        source=output/f"wrapper_{key}.cpp";source.write_text(wrapper_source(variant))
        obj=output/(f"wrapper_{key}"+(".obj" if config["msvc"] else ".o"))
        compilation=prior.compile_object(config,source,obj,output,output/f"compile_{key}.log");objects.append(obj)
        variants.append({**variant,"header":header.name,"header_sha256":sha256(header),"header_metadata":header_metadata(header),
                         "encoded_bodies":encoded_bodies(header) if variant["encoded"] else None,
                         "emission_metadata_sha256":sha256(metadata),"emission_command":command,
                         "emission_process_wall_ms":emission_ms,"emission":emitted,"compilation":compilation})
    body_matches={}
    for semantic in ("residual","care"):
        chosen=[v for v in variants if v["key"].startswith(semantic+"_")]
        if len(chosen)!=3 or not all(v["encoded_bodies"]==chosen[0]["encoded_bodies"] for v in chosen[1:]):
            raise RuntimeError("binding experiment changed encoded bodies/consumer slots: "+semantic)
        body_matches[semantic]={"all_three_exactly_equal":True,**chosen[0]["encoded_bodies"]}
    keys=[v["key"] for v in VARIANTS]
    declarations="\n".join(f"void register_{key}(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);" for key in keys)
    registrations="\n".join(f"        register_{key}(candidates,aos.span(),soa.span(),rows,channels,packed);" for key in keys)
    binding_declarations="\n".join(f"void register_binding_{key}(std::vector<BindingCandidate>&,const suffice::PackedDataset&);" for key in BINDING_VARIANTS)
    binding_registrations="\n".join(f"        register_binding_{key}(bindings,packed);" for key in BINDING_VARIANTS)
    source=output/"driver.cpp";source.write_text(driver_source(declarations,registrations,binding_declarations,binding_registrations,args.functional_pilot))
    suffix=".obj" if config["msvc"] else ".o"
    obj=output/("driver"+suffix);driver=prior.compile_object(config,source,obj,output,output/"compile_driver.log");objects.append(obj)
    source=output/"binding_sink.cpp";source.write_text(SINK)
    obj=output/("binding_sink"+suffix);sink=prior.compile_object(config,source,obj,output,output/"compile_binding_sink.log");objects.append(obj)
    executable=output/("comparison.exe" if config["msvc"] else "comparison")
    link=([config["executable"],"/nologo",*map(str,objects),str(library),"/Fe:"+str(executable)] if config["msvc"] else
          [config["executable"],*config["flags"],*map(str,objects),str(library),"-o",str(executable)])
    link_ms=execute(link,output/"link.log",cwd=output)
    command=[str(executable),str(args.raw.resolve()),str(args.packed.resolve()),program,str(threshold),
             str(args.repeat),str(output/"measurements.json"),str(BINDING_PASSES)]
    execution_ms=measurements=None
    if not args.prepare_only:
        execution_ms=execute(command,output/"execution.log",cwd=ROOT)
        measurements=json.loads((output/"measurements.json").read_text())
        if {c["backend"] for c in measurements["candidates"]}!=set(CANDIDATES):raise RuntimeError("candidate inventory changed")
    if initial!={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
                 "program_sha256":sha256(program) if Path(program).is_file() else None}:raise RuntimeError("benchmark input changed")
    for relative,expected in harness.items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("harness changed")
    for label,binary in (("current",args.binary),("baseline",args.baseline_binary)):
        if sha256(binary)!=versions[label]["sha256"]:raise RuntimeError("generating executable changed")
    if compiler_config(args.compiler)!=config:raise RuntimeError("compiler identity changed")
    for relative,expected in support["identity"]["inputs"].items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("core input changed: "+relative)
    if source_identity(ROOT/"baseline/v0_7")!=frozen or frozen_release_identity()!=release:raise RuntimeError("frozen baseline changed")
    result={"schema_version":8,"status":"prepared" if args.prepare_only else "completed",
            "measurement_purpose":"functional_pipeline_pilot" if args.functional_pilot else "fixed_performance_experiment",
            "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "environment":{"platform":platform.platform(),"python":platform.python_version(),"cpu":cpu_identity(),
                           "available_affinity_before":available,"benchmark_affinity":affinity},
            "binaries":versions,"compiler":config,"inputs":initial,"harness_sources":harness,"fixed_prefix_bits":DEPTH,
            "fixed_proof_budgets":PROOF_BUDGETS,"binding_passes_per_sample":BINDING_PASSES,
            "candidate_inventory":list(CANDIDATES),"binding_inventory":list(BINDING_NAMES),
            "variants":variants,"encoded_body_matches":body_matches,"shared_support_library":support,
            "frozen_sources":frozen,"frozen_release":release,"driver_compilation":driver,"sink_compilation":sink,
            "interface_sha256":sha256(output/"benchmark_interface.hpp"),
            "link":{"argv":link,"wall_ms":link_ms,"binary_sha256":sha256(executable)},
            "execution":{"argv":command,"process_wall_ms":execution_ms},"measurements":measurements,
            "scope":[
                "Ten full-scan candidates, separate translation units, no requested LTO; all compilation before measured execution.",
                "Frozen0.7 emitter and current0.8 emitter use the same host native compiler and shared packed reader; not a whole-process old/new comparison.",
                "Pressure/guarded semantic plans and exact encoded arithmetic bodies/slot identities are checked equal across frozen/single/grouped treatments.",
                "Whole-mask and arbitrary-range results use an original arithmetic-IR oracle, checked outside fresh timing.",
                "Exact integer nanosecond samples are retained; medians and same-round ratios are descriptive, not significance tests.",
                "Binding-only samples perform32 full binding passes plus an out-of-line sink consuming every original slot pointer/size; include loop and checksum overhead, do not subtract from full scans.",
                "Untimed forwarding-Dataset probes count actual view calls/requested planes/returned extents and check allocation-free exact output; bytes are logical spans, not DRAM traffic.",
                "Input packing, oracle construction, copies, generation, compilation and preparation are reported separately.",
                "No portfolio retuning, production AI workload, native Windows validation, global novelty or hardware-optimality claim."
            ]}
    prior.previous.write_json(output/"result.json",result)
    print(json.dumps({"output":str(output),"status":result["status"],"candidates":len(CANDIDATES),
                      "encoded_bodies_equal":True,"exact":measurements["exact"] if measurements else None},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
