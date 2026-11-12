#!/usr/bin/env python3
"""Matched-depth v0.5 comparison, including unchanged frozen v0.4 in one process.

All source emission and compilation finish before fresh execution timing. The
frozen portable library is compiled from unchanged sources with only its C++
namespace renamed by -Dsuffice=suffice_v04 (MSVC /D equivalent).
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

import benchmark_v0_4 as previous
from codegen_support import ROOT, compiler_config, ensure_support, execute, sha256

DEPTH = 12
VARIANTS = (
    {"key": "frozen_frontier", "namespace": "bench_frozen04_frontier", "baseline": True,
     "flags": ["--encoded-strategy", "frontier", "--encoded-prefix-bits", "12", "--encoded-schedule", "pressure"],
     "raw": False, "encoded": True, "label": "v04-generated-frontier12", "prefix": 12},
    {"key": "eager", "namespace": "bench_eager05", "baseline": False,
     "flags": ["--encoded-strategy", "eager", "--encoded-schedule", "pressure"],
     "raw": True, "encoded": True, "label": "v05-generated-eager"},
    {"key": "soa", "namespace": "bench_soa05", "baseline": False,
     "flags": ["--raw-layout", "planar", "--encoded-strategy", "eager", "--encoded-schedule", "pressure"],
     "raw": True, "encoded": False, "label": "raw-soa"},
    {"key": "frontier", "namespace": "bench_frontier05", "baseline": False,
     "flags": ["--encoded-strategy", "frontier", "--encoded-prefix-bits", "12", "--encoded-schedule", "pressure"],
     "raw": False, "encoded": True, "label": "v05-generated-frontier12", "prefix": 12},
    {"key": "residual", "namespace": "bench_residual05", "baseline": False,
     "flags": ["--encoded-strategy", "residual", "--encoded-prefix-bits", "12", "--encoded-schedule", "pressure"],
     "raw": False, "encoded": True, "label": "v05-generated-residual12", "prefix": 12},
)

# Deliberately has no Suffice types. The frozen and current libraries expose
# only this common harness ABI; their data/plan/executor types never mix.
INTERFACE = r'''
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
struct PrefixDiagnostic {
    bool available{}, from_execution_counters{};
    std::uint64_t vector_batches{}, fallback_batches{}, scalar_batches{}, scalar_fallback_batches{};
    std::uint64_t vector_rows{}, scalar_or_tail_rows{}, constant_fastpath_rows{};
    std::uint64_t certified_rows{}, ambiguous_rows{}, bounds_checked_rows{}, physical_refined_rows{};
    std::uint64_t dirty_words{}, fallback_words{}, dense_tiles{}, sparse_words{}, scratch_bytes{};
};
struct BenchRunner {
    virtual ~BenchRunner()=default;
    virtual std::uint64_t run(std::span<std::uint64_t>)=0;
    virtual std::uint64_t run_range(std::uint64_t,std::uint64_t,std::span<std::uint64_t>)=0;
    virtual bool uses_simd() const noexcept=0;
    virtual bool simd_known() const noexcept { return true; }
    virtual PrefixDiagnostic diagnose(std::span<const std::uint64_t>) { return {}; }
};
struct Candidate {
    std::string name,representation,implementation;
    std::unique_ptr<BenchRunner> runner;
    std::vector<double> samples;
    double prepare_ms{};
    PrefixDiagnostic diagnostic;
};
using Native=std::span<const std::int32_t>;
'''

DIAGNOSTIC = r'''
    PrefixDiagnostic diagnose(std::span<const std::uint64_t> expected) override {
        PrefixDiagnostic result;result.available=true;
        const auto& data=data_;
        // Reconstruct exactly the generated full-run branch geometry, outside
        // timing. These are logical masks and physical execution widths, never
        // hardware load/cache/DRAM measurements.
        std::array<std::span<const std::uint8_t>,generated::encoded_plane_count> planes{};
        const auto check=[&](std::uint64_t global,std::uint32_t offset,unsigned count) {
            const auto bounds=generated::evaluate_encoded_prefix_unchecked(planes,offset,count);
            const auto valid=count==64?UINT64_MAX:(std::uint64_t{1}<<count)-1;
            auto truth=expected[static_cast<std::size_t>(global/64)]>>(global%64);
            if(global%64 && count>64-global%64)
                truth|=expected[static_cast<std::size_t>(global/64+1)]<<(64-global%64);
            truth&=valid;
            if((bounds.must&~bounds.may) || (bounds.must&~truth) || (truth&~bounds.may)
               || ((bounds.must|bounds.may)&~valid))
                throw std::runtime_error("generated prefix diagnostic violates original-IR bounds");
            const auto unknown=bounds.may^bounds.must;
            const auto ambiguous=static_cast<unsigned>(std::popcount(unknown));
            result.ambiguous_rows+=ambiguous;result.certified_rows+=count-ambiguous;
            result.bounds_checked_rows+=count;
            result.dirty_words+=unknown!=0;
            return unknown!=0;
        };
        if constexpr(generated::encoded_plane_count==0) {
            for(std::uint64_t first=0;first<data.info().rows;first+=64)
                check(first,0,static_cast<unsigned>(std::min<std::uint64_t>(64,data.info().rows-first)));
            result.constant_fastpath_rows=data.info().rows;
            return result;
        }
        for(std::uint64_t block=0;block<data.block_count();++block) {
            const auto length=data.block_length(block);
            const auto first=block*data.info().block_rows;
            for(std::size_t i=0;i<planes.size();++i) {
                const auto contract=generated::plane_contracts[i];
                planes[i]=data.plane_view(block,contract.channel,contract.bit,1);
            }
            const auto vector_end=kernel_.uses_simd() && first%64==0 ? length&~std::uint32_t{255}:0;
            for(std::uint32_t offset=0;offset<vector_end;offset+=256) {
                bool dirty=false;
                for(unsigned word=0;word<4;++word)
                    dirty=check(first+offset+word*64,offset+word*64,64)||dirty;
                ++result.vector_batches;result.vector_rows+=256;
                if(dirty){++result.fallback_batches;result.physical_refined_rows+=256;result.fallback_words+=4;}
            }
            for(auto offset=vector_end;offset<length;) {
                const auto take=std::min<unsigned>(64,length-offset);
                const bool dirty=check(first+offset,offset,take);
                ++result.scalar_batches;result.scalar_or_tail_rows+=take;
                if(dirty){++result.scalar_fallback_batches;result.physical_refined_rows+=take;++result.fallback_words;}
                offset+=take;
            }
        }
        return result;
    }
'''

WRAPPER = previous.WRAPPER.replace('#include "kernel_@KEY@.hpp"', '#include "suffice/storage.hpp"\n#include "kernel_@KEY@.hpp"')
WRAPPER = WRAPPER.replace('    generated::PreparedEncodedKernel<suffice::PackedDataset> kernel_;',
                          '    const suffice::PackedDataset& data_;\n    generated::PreparedEncodedKernel<suffice::PackedDataset> kernel_;')
WRAPPER = WRAPPER.replace(':kernel_(data,true,true){}', ':data_(data),kernel_(data,true,true){}')

CORE = r'''
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
    PrefixDiagnostic diagnose(std::span<const std::uint64_t> expected) override {
        if constexpr(std::is_same_v<Executor,suffice::FrontierExecutor>) {
            std::vector<std::uint64_t> mask(expected.size(),UINT64_MAX);
            const auto stats=executor_.run(mask);
            if(!std::equal(mask.begin(),mask.end(),expected.begin(),expected.end()))
                throw std::runtime_error("portable diagnostic differs from original IR");
            PrefixDiagnostic result;result.available=true;result.from_execution_counters=true;
            result.certified_rows=stats.frontier_prefix_resolved_rows;
            result.ambiguous_rows=stats.refined_rows;result.bounds_checked_rows=stats.rows;
            result.dirty_words=stats.frontier_dirty_words;result.fallback_words=stats.frontier_fallback_words;
            result.physical_refined_rows=stats.frontier_fallback_rows;
            result.dense_tiles=stats.frontier_dense_tiles;result.sparse_words=stats.frontier_sparse_words;
            result.scratch_bytes=stats.peak_scratch_bytes;
            return result;
        } else return {};
    }
};
'''
FROZEN_WRAPPER = r'''
#include "benchmark_interface.hpp"
#include "suffice/ir.hpp"
#include "suffice/storage.hpp"
#include "suffice/predicate.hpp"
#include "suffice/frontier.hpp"
#include "suffice/frontier_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace {
class Frozen final:public BenchRunner {
    suffice::PackedDataset data_;
    suffice::FrontierPlan plan_;
    std::unique_ptr<suffice::FrontierExecutor> executor_;
public:
    Frozen(const char* path,const char* specification,std::int64_t threshold):data_(path) {
        data_.verify_payload();
        std::ifstream source(specification);const bool custom=source.good();
        const auto text=custom?std::string(std::istreambuf_iterator<char>(source),{}):std::string{};
        auto program=custom?suffice::parse_program(text):suffice::builtin_program(specification,threshold);
        std::vector<suffice::Bounds> ranges;
        for(auto range:data_.info().ranges)ranges.push_back({range.min,range.max});
        plan_=suffice::compile_frontier(suffice::compile_predicate(program.compile(ranges)),12);
        if(!plan_.complete)throw std::runtime_error("frozen portable frontier incomplete");
        executor_=std::make_unique<suffice::FrontierExecutor>(data_,plan_,128,true);
    }
    std::uint64_t run(std::span<std::uint64_t> mask) override {return executor_->run(mask).accepted;}
    std::uint64_t run_range(std::uint64_t first,std::uint64_t count,std::span<std::uint64_t> mask) override {return executor_->run_range(first,count,mask).accepted;}
    bool uses_simd()const noexcept override {return executor_->uses_simd();}
    PrefixDiagnostic diagnose(std::span<const std::uint64_t> expected)override {
        std::vector<std::uint64_t> mask(expected.size(),UINT64_MAX);
        const auto stats=executor_->run(mask);
        if(!std::equal(mask.begin(),mask.end(),expected.begin(),expected.end()))
            throw std::runtime_error("frozen portable diagnostic differs from original IR");
        PrefixDiagnostic result;result.available=true;result.from_execution_counters=true;
        result.certified_rows=stats.frontier_prefix_resolved_rows;result.ambiguous_rows=stats.refined_rows;
        result.bounds_checked_rows=stats.rows;result.dirty_words=stats.frontier_fallback_words;
        result.fallback_words=stats.frontier_fallback_words;result.sparse_words=stats.frontier_fallback_words;
        // Old runtime has no physical-row counter. Independently reconstruct
        // exact tail widths from prefix bounds instead of asserting words*64.
        const auto& prefix=plan_.prefix;
        std::vector<std::uint64_t> values(prefix.nodes.size());
        for(std::uint64_t b=0;b<data_.block_count();++b) {
            const auto length=data_.block_length(b);
            for(std::uint32_t offset=0;offset<length;offset+=64) {
                const auto take=std::min<unsigned>(64,length-offset);
                const auto valid=take==64?UINT64_MAX:(std::uint64_t{1}<<take)-1;
                for(std::size_t i=0;i<prefix.nodes.size();++i) {
                    const auto& n=prefix.nodes[i];
                    switch(n.op) {
                    case suffice::WordcodeOp::False:values[i]=0;break;
                    case suffice::WordcodeOp::True:values[i]=valid;break;
                    case suffice::WordcodeOp::Plane:{
                        const auto bytes=data_.plane_view(b,prefix.loads[n.load_index].channel,n.bit,1);
                        std::uint64_t word=0;
                        for(unsigned k=0;k<take;++k)
                            word|=std::uint64_t((bytes[(offset+k)/8]>>((offset+k)%8))&1U)<<k;
                        values[i]=word;break;}
                    case suffice::WordcodeOp::And:values[i]=values[n.a]&values[n.b];break;
                    case suffice::WordcodeOp::Or:values[i]=values[n.a]|values[n.b];break;
                    case suffice::WordcodeOp::Not:values[i]=~values[n.a]&valid;break;
                    }
                }
                if(values[prefix.may_root]^values[prefix.must_root])result.physical_refined_rows+=take;
            }
        }
        result.scratch_bytes=stats.peak_scratch_bytes;
        return result;
    }
};
}
void register_frozen_portable(std::vector<Candidate>& candidates,const char* path,const char* program,std::int64_t threshold) {
    const auto start=std::chrono::steady_clock::now();
    auto runner=std::make_unique<Frozen>(path,program,threshold);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    candidates.push_back({"v04-portable-frontier12","packed","frozen-core-namespace-only",std::move(runner),{},elapsed,{}});
}
'''


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("retained v0.4 template changed: " + old[:80])
    return text.replace(old, new)


def driver_source(declarations, registrations):
    source = previous.DRIVER
    begin = source.index('template<class Executor> class Core')
    end = source.index('template<class Executor,class... Args>', begin)
    source = source[:begin] + CORE + source[end:]
    source = replace_once(source, '#include <new>', '#include <new>\n#include <type_traits>\n' + declarations)
    source = replace_once(source, '@REGISTER_VARIANTS@', registrations)
    source = replace_once(source, '        if(predicate.complete)add_core<suffice::EncodedPredicateExecutor>(candidates,"encoded-predicate","packed",packed,predicate);', '')
    source = replace_once(source, '"encoded-wordcode"', '"v05-portable-wordcode"')
    source = replace_once(source, '        if(frontier.complete)add_core<suffice::FrontierExecutor>(candidates,"encoded-frontier12","packed",packed,frontier);', r'''
        if(frontier.complete) {
            add_core<suffice::FrontierExecutor>(candidates,"v05-portable-sparse12","packed",packed,frontier,128,true,
                suffice::FrontierRepairOptions{suffice::FrontierRepairPolicy::SparseWords,25});
            add_core<suffice::FrontierExecutor>(candidates,"v05-portable-hybrid12","packed",packed,frontier,128,true,
                suffice::FrontierRepairOptions{suffice::FrontierRepairPolicy::Hybrid,25});
        }
        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));''')
    source = replace_once(source, 'candidate.runner->diagnose(packed,expected)', 'candidate.runner->diagnose(expected)')
    source = replace_once(source, 'schema_version\\\":4', 'schema_version\\\":5')
    source = replace_once(source, '0x5355464649434534ULL', '0x5355464649434535ULL')
    source = replace_once(source, '<<",\\\"scalar_or_tail_rows\\\":"<<c.diagnostic.scalar_or_tail_rows<<"}}";', r'''<<",\"scalar_or_tail_rows\":"<<c.diagnostic.scalar_or_tail_rows
                  <<",\"from_execution_counters\":"<<(c.diagnostic.from_execution_counters?"true":"false")
                  <<",\"scalar_batches\":"<<c.diagnostic.scalar_batches<<",\"scalar_fallback_batches\":"<<c.diagnostic.scalar_fallback_batches
                  <<",\"vector_rows\":"<<c.diagnostic.vector_rows<<",\"constant_fastpath_rows\":"<<c.diagnostic.constant_fastpath_rows
                  <<",\"physical_refined_rows\":"<<c.diagnostic.physical_refined_rows
                  <<",\"dirty_words\":"<<c.diagnostic.dirty_words<<",\"fallback_words\":"<<c.diagnostic.fallback_words
                  <<",\"dense_tiles\":"<<c.diagnostic.dense_tiles<<",\"sparse_words\":"<<c.diagnostic.sparse_words
                  <<",\"scratch_bytes\":"<<c.diagnostic.scratch_bytes<<"}}";''')
    return source


def wrapper_source(variant):
    registration = []
    if variant["raw"]:
        planar = variant["key"] == "soa"
        layout, representation = ("soa", "planar") if planar else ("aos", "interleaved")
        for simd in (False, True):
            name = "raw-" + layout + ("-simd" if simd else "-scalar")
            registration.append(f'    add(candidates,"{name}","{representation}","generated",[&]{{return std::make_unique<Raw>({layout},rows,channels,{str(simd).lower()});}});')
    if variant["encoded"]:
        registration.append(f'    if constexpr(generated::encoded_emitted)add(candidates,"{variant["label"]}","packed","generated",[&]{{return std::make_unique<Encoded>(packed);}});')
    return (WRAPPER.replace("@KEY@", variant["key"]).replace("@NAMESPACE@", variant["namespace"])
            .replace("@DIAGNOSTIC@", DIAGNOSTIC if "prefix" in variant else "")
            .replace("@REGISTER@", "\n".join(registration)))


def default_baseline():
    return ROOT / "baseline/v0_4/build" / ("suffice.exe" if os.name == "nt" else "suffice")


def source_identity(root):
    paths = sorted(p for p in (root / "src").glob("*.cpp") if p.name != "main.cpp")
    paths += sorted((root / "include").rglob("*.hpp"))
    if not paths:
        raise RuntimeError("frozen source tree is missing")
    return {p.relative_to(root).as_posix(): sha256(p) for p in paths}


def compile_object(config, source, obj, include, log, source_root=ROOT, rename=False):
    if config["msvc"]:
        argv = [config["executable"], *config["flags"], "/I"+str(source_root/"include"), "/I"+str(include)]
        if rename: argv.append("/Dsuffice=suffice_v04")
        argv += ["/c", str(source), "/Fo"+str(obj)]
    else:
        argv = [config["executable"], *config["flags"], "-I", str(source_root/"include"), "-I", str(include)]
        if rename: argv.append("-Dsuffice=suffice_v04")
        argv += ["-c", str(source), "-o", str(obj)]
    wall = execute(argv, log, cwd=include)
    return {"argv": argv, "wall_ms": wall, "source_sha256": sha256(source), "object_sha256": sha256(obj)}


def ensure_frozen_support(config, root, cache_parent=None):
    inputs = source_identity(root)
    transform = "/Dsuffice=suffice_v04" if config["msvc"] else "-Dsuffice=suffice_v04"
    identity = {"compiler": config, "inputs": inputs, "namespace_only_transform": transform}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    cache = Path(cache_parent or ROOT/"build/codegen_cache")/("frozen04-"+key)
    manifest = cache/"manifest.json"
    library = cache/("suffice_v04.lib" if config["msvc"] else "libsuffice_v04.a")
    if manifest.is_file() and library.is_file():
        record = json.loads(manifest.read_text(encoding="utf-8"))
        if record["library_sha256"] != sha256(library):
            raise RuntimeError("frozen archive integrity failed")
        return library, {**record, "cache_hit": True, "this_invocation_build_ms": 0.0}
    cache.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    records, objects = [], []
    for relative in inputs:
        if not relative.startswith("src/"): continue
        source = root/relative
        obj = cache/(source.stem+(".obj" if config["msvc"] else ".o"))
        records.append(compile_object(config, source, obj, cache, cache/(source.stem+".log"), root, True))
        objects.append(obj)
    archiver = shutil.which("lib.exe" if config["msvc"] else "ar")
    if not archiver: raise RuntimeError("static archive tool missing")
    argv = ([archiver, "/nologo", "/OUT:"+str(library), *map(str, objects)] if config["msvc"]
            else [archiver, "rcs", str(library), *map(str, objects)])
    archive_ms = execute(argv, cache/"archive.log", cwd=cache)
    if source_identity(root) != inputs:
        raise RuntimeError("frozen sources changed during compilation")
    elapsed = (time.perf_counter()-start)*1000
    record = {"cache_key": key, "identity": identity, "compilations": records, "archive_argv": argv,
              "archive_ms": archive_ms, "original_build_ms": elapsed, "library_sha256": sha256(library)}
    previous.write_json(manifest, record)
    return library, {**record, "cache_hit": False, "this_invocation_build_ms": elapsed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    parser.add_argument("--baseline-binary", type=Path, default=default_baseline())
    parser.add_argument("--baseline-source", type=Path, default=ROOT/"baseline/v0_4")
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--packed", required=True, type=Path)
    parser.add_argument("--program", required=True)
    parser.add_argument("--threshold", type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeat", type=int, default=40)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--pin-cpu", default="auto")
    parser.add_argument("--prepare-only", action="store_true", help="Emit/compile only; defer all native measurements")
    args = parser.parse_args()
    if not 1 <= args.repeat <= 1000: parser.error("repeat must be 1..1000")
    for path in (args.binary, args.baseline_binary, args.raw, args.packed):
        if not path.is_file(): parser.error("required file missing: "+str(path))
    output=args.output.resolve()
    if output.exists(): parser.error("use a fresh output directory")
    available=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    if args.pin_cpu != "none" and available:
        cpu=available[0] if args.pin_cpu=="auto" else int(args.pin_cpu)
        if cpu not in available: parser.error("CPU outside permitted affinity")
        os.sched_setaffinity(0,{cpu})
    affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    program=str(Path(args.program).resolve()) if Path(args.program).is_file() else args.program
    defaults={"identity":0,"energy":1000000,"delta":100000,"fir3":100000}
    threshold=args.threshold if args.threshold is not None else defaults.get(program,0)
    output.mkdir(parents=True)
    versions={}
    for label,binary in (("current",args.binary),("baseline",args.baseline_binary)):
        version=subprocess.run([str(binary.resolve()),"--version"],capture_output=True,text=True,check=True).stdout.strip()
        versions[label]={"path":str(binary.resolve()),"sha256":sha256(binary),"version_output":version}
    if versions["baseline"]["version_output"]!="suffice 0.4.0" or versions["current"]["version_output"]!="suffice 0.5.0":
        raise RuntimeError("this comparison requires frozen0.4 and current0.5 executable identities")
    config=compiler_config(args.compiler)
    initial={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
             "program_sha256":sha256(program) if Path(program).is_file() else None}
    harness={str(Path(p).relative_to(ROOT)):sha256(p) for p in (__file__,previous.__file__,ROOT/"scripts/codegen_support.py")}
    library,support=ensure_support(config,args.cache)
    frozen_root=args.baseline_source.resolve()
    frozen_library,frozen_support=ensure_frozen_support(config,frozen_root,args.cache)
    (output/"benchmark_interface.hpp").write_text(INTERFACE,encoding="utf-8")
    variants,objects=[],[]
    for variant in VARIANTS:
        key=variant["key"]
        header=output/f"kernel_{key}.hpp";metadata=output/f"emission_{key}.json"
        binary=args.baseline_binary if variant["baseline"] else args.binary
        command=[str(binary.resolve()),"emit-cpp","--input",str(args.packed.resolve()),"--program",program,
                 "--namespace",variant["namespace"],"--output",str(header),"--json",str(metadata),*variant["flags"]]
        if args.threshold is not None or program in defaults: command += ["--threshold",str(threshold)]
        emission_ms=execute(command,output/f"emission_{key}.log",cwd=ROOT)
        emitted=json.loads(metadata.read_text(encoding="utf-8"))
        if not emitted.get("encoded_emitted"):raise RuntimeError("focused benchmark needs complete encoded lowering")
        if key=="residual" and not emitted.get("encoded_residual_emitted"):
            raise RuntimeError("requested residual silently fell back; this comparison requires a residual kernel")
        source=output/f"wrapper_{key}.cpp";source.write_text(wrapper_source(variant),encoding="utf-8")
        obj=output/(f"wrapper_{key}.obj" if config["msvc"] else f"wrapper_{key}.o")
        compilation=compile_object(config,source,obj,output,output/f"compile_{key}.log")
        objects.append(obj)
        variants.append({**variant,"header":header.name,"header_sha256":sha256(header),"emission_command":command,
                         "emission_process_wall_ms":emission_ms,"emission":emitted,"compilation":compilation})
    source=output/"wrapper_frozen_portable.cpp";source.write_text(FROZEN_WRAPPER,encoding="utf-8")
    obj=output/("wrapper_frozen_portable.obj" if config["msvc"] else "wrapper_frozen_portable.o")
    frozen_compilation=compile_object(config,source,obj,output,output/"compile_frozen_portable.log",frozen_root,True)
    objects.append(obj)
    declarations="\n".join(f'void register_{v["key"]}(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);' for v in VARIANTS)
    declarations+='\nvoid register_frozen_portable(std::vector<Candidate>&,const char*,const char*,std::int64_t);'
    registrations="\n".join(f'        register_{v["key"]}(candidates,aos.span(),soa.span(),rows,channels,packed);' for v in VARIANTS)
    source=output/"driver.cpp";source.write_text(driver_source(declarations,registrations),encoding="utf-8")
    obj=output/("driver.obj" if config["msvc"] else "driver.o")
    driver_compilation=compile_object(config,source,obj,output,output/"compile_driver.log")
    objects.append(obj)
    executable=output/("comparison.exe" if config["msvc"] else "comparison")
    link=([config["executable"],"/nologo",*map(str,objects),str(library),str(frozen_library),"/Fe:"+str(executable)] if config["msvc"]
          else [config["executable"],*config["flags"],*map(str,objects),str(library),str(frozen_library),"-o",str(executable)])
    link_ms=execute(link,output/"link.log",cwd=output)
    command=[str(executable),str(args.raw.resolve()),str(args.packed.resolve()),program,str(threshold),str(args.repeat),str(output/"measurements.json")]
    execution_ms=None;measurements=None
    if not args.prepare_only:
        execution_ms=execute(command,output/"execution.log",cwd=ROOT)
        measurements=json.loads((output/"measurements.json").read_text(encoding="utf-8"))
    if initial!={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,"program_sha256":sha256(program) if Path(program).is_file() else None}:
        raise RuntimeError("benchmark input changed")
    for relative,expected in harness.items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("benchmark harness changed")
    for label,binary in (("current",args.binary),("baseline",args.baseline_binary)):
        if sha256(binary)!=versions[label]["sha256"]:raise RuntimeError("generating binary changed")
    if compiler_config(args.compiler)!=config:raise RuntimeError("compiler identity changed")
    for relative,expected in support["identity"]["inputs"].items():
        if sha256(ROOT/relative)!=expected:raise RuntimeError("core source changed: "+relative)
    if source_identity(frozen_root)!=frozen_support["identity"]["inputs"]:raise RuntimeError("frozen source changed")
    result={"schema_version":5,"status":"prepared" if args.prepare_only else "completed",
            "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "environment":{"platform":platform.platform(),"python":platform.python_version(),"available_affinity_before":available,"benchmark_affinity":affinity},
            "binaries":versions,"compiler":config,"inputs":initial,"harness_sources":harness,"fixed_prefix_bits":DEPTH,
            "variants":variants,"shared_support_library":support,"frozen_support_library":frozen_support,
            "frozen_portable_compilation":frozen_compilation,"driver_compilation":driver_compilation,
            "interface_sha256":sha256(output/"benchmark_interface.hpp"),
            "link":{"argv":link,"wall_ms":link_ms,"binary_sha256":sha256(executable)},
            "execution":{"argv":command,"process_wall_ms":execution_ms},"measurements":measurements,
            "scope":[
                "All native fresh timings use one process, identical masks, and separate no-LTO wrapper translation units.",
                "Frozen portable sources are unchanged: only the namespace macro is added to avoid ODR collisions. Compiler and flags match current sources.",
                "All complete and range outputs are checked against unspecialized original arithmetic IR, outside timers.",
                "Raw controls include generated AoS/SoA scalar/SIMD and core dense/raw-vector; copies and SoA transpose are outside warm execution and reported separately.",
                "Fixed depth12 is declared before measurement. No post-hoc depth or backend winner is a deployed planner.",
                "Generated diagnostics reconstruct exact 64/256 repair branch geometry outside timing. Portable counters come from untimed execution; old physical tail widths are independently reconstructed.",
                "Frozen portable preparation includes a second mapping, validation and plan build; it is not directly comparable with current executor-only construction cost.",
                "Generated kernels remain outside automatic selection. Selection-policy experiments and regret are separate CLI comparisons.",
                "Shared libraries, all representations and all candidate instances coexist. Process RSS is a research harness measurement, not minimal deployment memory.",
                "Scalar disables explicit SIMD; host compiler autovectorization remains enabled. This experiment establishes neither Windows nor external-engine superiority."
            ]}
    previous.write_json(output/"result.json",result)
    print(json.dumps({"output":str(output),"status":result["status"],"exact":measurements["exact"] if measurements else None,
                      "candidates":len(measurements["candidates"]) if measurements else None,
                      "fresh_mask_checks":measurements["fresh_mask_checks"] if measurements else None},indent=2))


if __name__=="__main__":
    try: main()
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
