#!/usr/bin/env python3
"""Prepare and measure bounded repair policies across fresh processes/link orders.

No automatic winner promotion. Exact arithmetic masks, duplicate controls,
dirty-word histograms, integer samples, per-session environment identity and
all preparation costs are retained. All builds finish before measurement.
"""
from __future__ import annotations
import argparse
from collections import Counter
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import struct
import subprocess
import sys
import time

import benchmark_v0_5 as prior
from benchmark_v0_8 import allocation_source, cpu_identity, encoded_bodies
from benchmark_v0_2 import PRESETS
from codegen_support import ROOT, compiler_config, ensure_support, execute, sha256
from reproduce_v0_4 import prefix_stress

VARIANTS = [
    {"key":sem+"_"+policy, "namespace":"bench09_"+sem+"_"+policy,
     "baseline":False, "raw":False, "encoded":True, "label":sem+"-"+policy,
     "prefix":12, "policy":"batch256" if policy=="duplicate" else policy,
     "flags":["--encoded-strategy",strategy,"--encoded-prefix-bits","12",
              "--encoded-schedule","bounds-first","--encoded-carry-placement","recompute",
              "--encoded-plane-binding","grouped","--encoded-repair-policy",
              "batch256" if policy=="duplicate" else policy]}
    for sem,strategy in (("all","residual"),("care","guarded-residual"))
    for policy in ("batch256","word64","hybrid","duplicate")
] + [
    {"key":layout,"namespace":"bench09_raw_"+layout,"baseline":False,
     "raw":True,"encoded":False,"label":"raw-"+layout,
     "flags":["--encoded-strategy","eager"]+(["--raw-layout","planar"] if layout=="soa" else [])}
    for layout in ("aos","soa")
]
CANDIDATES = [v["label"] for v in VARIANTS if v["encoded"]] + [
    "raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd","portable-wordcode"]
PROGRAMS = {
 "magnitude": (2, PRESETS["magnitude-intersection"][1]),
 "spatial": (2, "input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 1000\n"),
 "overlap": (2, (ROOT/"examples/predicate_overlapping_gates.suf").read_text()),
 "and4": (4, "input a0 0 0\ninput a1 1 0\ninput a2 2 0\ninput a3 3 0\nmin p0 a0 a1\nmin p1 a2 a3\nmin root p0 p1\noutput root > 100000000\n"),
}

def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def write(path, value):
    path=Path(path); temp=path.with_name(path.name+".tmp")
    temp.write_text(json.dumps(value,indent=2)+"\n"); temp.replace(path)

def source_identity():
    paths=[]
    for directory,pattern in (("src","*.cpp"),("include","*.hpp"),("scripts","*.py"),("tests","*.cpp"),("tests","*.hpp")):
        paths.extend((ROOT/directory).rglob(pattern))
    return {str(p.relative_to(ROOT)):sha256(p) for p in sorted(set(paths))}

def environment():
    return {"cpu":cpu_identity(),"os":platform.platform(),"machine":platform.machine(),
            "python":platform.python_version(),
            "affinity":sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None}

def interface_source():
    text=prior.INTERFACE.replace("#include <cstdint>","#include <cstdint>\n#include <array>")
    text=text.replace("    bool available{}, from_execution_counters{};",
                      "    std::array<std::uint64_t,5> dirty_histogram{};\n    bool available{}, from_execution_counters{};")
    text=text.replace("    PrefixDiagnostic diagnostic;\n",
                      "    PrefixDiagnostic diagnostic;\n    std::vector<std::uint64_t> samples_ns{};\n")
    return text

def wrapper_source(v):
    source=prior.wrapper_source(v)
    if v["encoded"]:
        d=prior.DIAGNOSTIC
        d=d.replace("bool dirty=false;","unsigned dirty=0;")
        d=d.replace("dirty=check(first+offset+word*64,offset+word*64,64)||dirty;",
                    "dirty+=check(first+offset+word*64,offset+word*64,64);")
        d=d.replace("++result.vector_batches;result.vector_rows+=256;",
                    "++result.vector_batches;result.vector_rows+=256;++result.dirty_histogram[dirty];")
        if v["policy"]=="word64":
            work="64*dirty"; words="dirty"
        elif v["policy"]=="hybrid":
            work="(dirty==1?64:256)"; words="(dirty==1?1:4)"
        else:
            work="256";words="4"
        d=d.replace("result.physical_refined_rows+=256;result.fallback_words+=4;",
                    "result.physical_refined_rows+="+work+";result.fallback_words+="+words+";")
        source=prior.replace_once(source,prior.DIAGNOSTIC,d)
    return source

def driver_source():
    declarations="\n".join("void register_"+v["key"]+"(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);" for v in VARIANTS)
    registrations="\n".join("        register_"+v["key"]+"(candidates,aos.span(),soa.span(),rows,channels,packed);" for v in VARIANTS)
    source=prior.driver_source(declarations,registrations)
    for line in (
      '        add_core<suffice::DenseExecutor>(candidates,"dense","interleaved",raw,plan,custom?std::string{}:std::string(argv[3]),true,true);',
      '        add_core<suffice::VectorExecutor>(candidates,"raw-vector","interleaved",raw,plan);'):
        source=prior.replace_once(source,line,"")
    a=source.index('        if(frontier.complete) {\n            add_core<suffice::FrontierExecutor>')
    b=source.index('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));',a)+len('        register_frozen_portable(candidates,argv[2],argv[3],std::stoll(argv[4]));')
    source=source[:a]+source[b:]
    source=source.replace('        auto frontier=suffice::compile_frontier(predicate,12);\n',"")
    source=source.replace('"v05-portable-wordcode"','"portable-wordcode"')
    source=prior.replace_once(source,'using Clock=std::chrono::steady_clock;', allocation_source()+r'''
using Clock=std::chrono::steady_clock;
static_assert(Clock::is_steady);
std::uint64_t ns_since(Clock::time_point p) {
    const auto n=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-p).count();
    if(n<=0)throw std::runtime_error("nonpositive timer");
    return static_cast<std::uint64_t>(n);
}
''')
    source=source.replace('if(argc!=7)throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD MIN_REPEAT OUTPUT_JSON");',
                          'if(argc!=8)throw std::invalid_argument("driver RAW PACKED PROGRAM THRESHOLD MIN_REPEAT OUTPUT_JSON SEED");')
    source=source.replace('        for(auto& candidate:candidates)candidate.diagnostic=candidate.runner->diagnose(expected);',r'''
        if(candidates.size()!=13)throw std::runtime_error("candidate inventory differs");
        std::uint64_t allocation_checks=0;
        for(auto& candidate:candidates) {
            candidate.diagnostic=candidate.runner->diagnose(expected);
            std::fill(output.begin(),output.end(),UINT64_MAX);
            allocation_probe::start();
            const auto accepted=candidate.runner->run(output);
            const auto allocations=allocation_probe::stop();
            verify(candidate,accepted);
            if(allocations)throw std::runtime_error("prepared run allocated");
            ++allocation_checks;
        }
''')
    source=source.replace('        for(auto& c:candidates)c.samples.reserve(repeat);',
                          '        for(auto& c:candidates){c.samples.reserve(repeat);c.samples_ns.reserve(repeat);}')
    source=source.replace('constexpr std::uint64_t timing_seed=0x5355464649434535ULL;',
                          'const std::uint64_t timing_seed=std::stoull(argv[7]);')
    source=source.replace('phase=Clock::now();const auto accepted=candidate.runner->run(output);const auto elapsed=ms(phase);',
                          'phase=Clock::now();const auto accepted=candidate.runner->run(output);const auto ns=ns_since(phase);const auto elapsed=double(ns)/1000000.0;')
    source=source.replace('candidate.samples.push_back(elapsed);++checks;',
                          'candidate.samples.push_back(elapsed);candidate.samples_ns.push_back(ns);++checks;')
    source=source.replace('std::setprecision(12)','std::setprecision(std::numeric_limits<double>::max_digits10)')
    source=source.replace('schema_version\\":5','schema_version\\":9')
    source=source.replace('<<",\\"range_mask_checks\\":"<<range_checks',
                          '<<",\\"allocation_checks\\":"<<allocation_checks<<",\\"range_mask_checks\\":"<<range_checks')
    source=source.replace('            result<<"],\\"prefix_diagnostic\\":{\\"available\\":"',
                          '''            result<<"],\\"samples_ns\\":[";
            for(std::size_t j=0;j<c.samples_ns.size();++j){if(j)result<<',';result<<c.samples_ns[j];}
            result<<"],\\"dirty_histogram\\":[";
            for(unsigned j=0;j<5;++j){if(j)result<<',';result<<c.diagnostic.dirty_histogram[j];}
            result<<"],\\"prefix_diagnostic\\":{\\"available\\":"''')
    source=source.replace('}catch(const std::exception& e){std::cerr<<e.what()',
                          '}catch(const std::exception& e){allocation_probe::stop();std::cerr<<e.what()')
    if 'samples_ns\\"' not in source or 'std::stoull(argv[7])' not in source or '"portable-wordcode"' not in source:
        raise RuntimeError("driver template adaptation incomplete")
    return source

def make_cases(rows):
    return [
      {"name":"magnitude_uniform","family":"magnitude","pattern":"uniform","rows":rows},
      {"name":"magnitude_heterogeneous","family":"magnitude","pattern":"heterogeneous","rows":rows},
      {"name":"magnitude_prefix_tail","family":"magnitude","pattern":"prefix_stress","rows":rows+17},
      {"name":"overlap_uniform","family":"overlap","pattern":"uniform","rows":rows},
      {"name":"and4_uniform","family":"and4","pattern":"uniform","rows":rows},
      *({"name":"spatial_"+p,"family":"spatial","pattern":p,"rows":rows}
        for p in ("clear","one","two","three","four","dense","clustered","mixed"))
    ]

def create_spatial(path,rows,pattern):
    """One uncertain row per dirty word; dense and clustered are separate controls."""
    arr=bytearray(rows*8); uncertain=0
    fixed={"clear":0,"one":1,"two":3,"three":7,"four":15}
    multiset=Counter()
    for i in range(rows):
        group=i//256; word=(i%256)//64
        if pattern in fixed: dirty=bool(fixed[pattern]&(1<<word)) and i%64==0
        elif pattern=="mixed": dirty=bool((group%16)&(1<<word)) and i%64==0
        elif pattern=="dense": dirty=True
        elif pattern=="clustered": dirty=i<rows//256
        else: raise ValueError(pattern)
        # Equal 1000/1001 uncertain values for the spatial one/clustered pair.
        value=1000+(uncertain%2) if dirty else 1000000000
        if dirty:uncertain+=1
        struct.pack_into("<ii",arr,i*8,value,value);multiset[value]+=1
    path.write_bytes(arr)
    return {"uncertain_constructed":uncertain,"rows":rows,"value_counts":dict(sorted(multiset.items()))}

def compile_family(binary,config,library,family,directory):
    directory.mkdir(parents=True)
    channels,program=PROGRAMS[family]
    (directory/"program.suf").write_text(program)
    # Compile each family once against full signed-int32 channel envelopes.
    schema=directory/"schema.i32"
    schema.write_bytes(struct.pack("<"+"i"*(2*channels),*([-2147483648]*channels+[2147483647]*channels)))
    schema_pack=directory/"schema.sfp"
    execute([binary,"pack","--input",schema,"--output",schema_pack,"--channels",channels,
             "--block-rows",4096],directory/"schema_pack.log")
    (directory/"benchmark_interface.hpp").write_text(interface_source())
    variants=[]; objects=[]
    for v in VARIANTS:
        header=directory/("kernel_"+v["key"]+".hpp")
        metadata=directory/("emit_"+v["key"]+".json")
        emission_ms=execute([binary,"emit-cpp","--input",schema_pack,"--program",directory/"program.suf",
            "--namespace",v["namespace"],*v["flags"],"--output",header,"--json",metadata],
            directory/("emit_"+v["key"]+".log"))
        emitted=json.loads(metadata.read_text())
        validate_emission(v,emitted)
        source=directory/("wrapper_"+v["key"]+".cpp");source.write_text(wrapper_source(v))
        obj=directory/("wrapper_"+v["key"]+(".obj" if config["msvc"] else ".o"))
        record=prior.compile_object(config,source,obj,directory,directory/("compile_"+v["key"]+".log"))
        variants.append({**v,"generation_ms":emission_ms,"header_bytes":header.stat().st_size,
            "metadata":emitted,"compilation":record})
        objects.append(obj)
    # Duplicate controls differ only in generated namespace/registration labels.
    duplicate_bodies={}
    for sem in ("all","care"):
        a=encoded_bodies(directory/("kernel_"+sem+"_batch256.hpp"))
        b=encoded_bodies(directory/("kernel_"+sem+"_duplicate.hpp"))
        if a!=b:raise RuntimeError("duplicate control arithmetic differs")
        duplicate_bodies[sem]=a
    driver=directory/"driver.cpp";driver.write_text(driver_source())
    dobj=directory/("driver.obj" if config["msvc"] else "driver.o")
    driver_compile=prior.compile_object(config,driver,dobj,directory,directory/"compile_driver.log")
    layouts=[]
    for index,ordered in enumerate((objects,list(reversed(objects)))):
        exe=directory/("benchmark_layout"+str(index)+(".exe" if config["msvc"] else ""))
        if config["msvc"]:
            argv=[config["executable"],*config["flags"],str(dobj),*map(str,ordered),str(library),"/Fe:"+str(exe)]
        else:
            argv=[config["executable"],*config["flags"],str(dobj),*map(str,ordered),str(library),"-o",str(exe)]
        link_ms=execute(argv,directory/("link"+str(index)+".log"),cwd=directory)
        layouts.append({"index":index,"executable":str(exe),"argv":argv,"link_ms":link_ms,
                        "object_order":[p.name for p in ordered],"sha256":sha256(exe),"bytes":exe.stat().st_size})
    inventory={str(p.relative_to(directory)):sha256(p) for p in directory.iterdir() if p.is_file()}
    return {"family":family,"compile_envelope":"all channels full signed int32 range",
            "variants":variants,"driver_compilation":driver_compile,"layouts":layouts,
            "duplicate_boolean_bodies_equal":True,"duplicate_body_hashes":duplicate_bodies,
            "inventory":inventory}

def validate_emission(variant,emitted):
    if not variant["encoded"]:return
    required={"encoded_emitted":True,"encoded_avx2_emitted":True,
              "encoded_residual_emitted":True,"encoded_recompute_carry_emitted":True,
              "encoded_grouped_binding_emitted":True,"encoded_prefix_bits":12,
              "encoded_schedule":"bounds-first","encoded_carry_placement":"recompute",
              "encoded_plane_binding":"grouped","encoded_repair_policy":variant["policy"],
              "encoded_narrow_repair_emitted":variant["policy"]!="batch256",
              "encoded_word_repair_max_dirty":{"batch256":0,"word64":4,"hybrid":1}[variant["policy"]]}
    for key,value in required.items():
        if emitted.get(key)!=value:raise RuntimeError("emitted path differs: "+variant["key"]+":"+key)

def validate_measurement(m,case):
    if m["schema_version"]!=9 or not m["exact"] or m["rows"]!=case["rows"]:raise RuntimeError("measurement identity")
    candidates=m["candidates"]
    if [c["backend"] for c in candidates]!=CANDIDATES:raise RuntimeError("candidate inventory")
    n=len(candidates);r=m["repeat"]
    if r%26 or m["fresh_mask_checks"]!=n*r:raise RuntimeError("incomplete cycles/checks")
    if m["timing_cycle_rows"]!=26 or m["complete_timing_cycles"]!=r//26:raise RuntimeError("cycle metadata")
    if m["allocation_checks"]!=13 or m["range_mask_checks"]!=468:raise RuntimeError("validation coverage")
    orders=m["timing_order"]
    if len(orders)!=r:raise RuntimeError("order length")
    for order in orders:
        if sorted(order)!=sorted(CANDIDATES):raise RuntimeError("order is not a permutation")
    for start in range(0,r,26):
        positions=[Counter() for _ in range(n)];predecessors=Counter()
        for order in orders[start:start+26]:
            for pos,name in enumerate(order):positions[pos][name]+=1
            predecessors.update(zip(order,order[1:]))
        if any(set(c.values())!={2} or len(c)!=n for c in positions):raise RuntimeError("unbalanced positions")
        if set(predecessors.values())!={2} or len(predecessors)!=n*(n-1):raise RuntimeError("unbalanced predecessors")
    hist=None
    for c in candidates:
        if len(c["samples_ns"])!=r or any(type(t)!=int or t<=0 for t in c["samples_ns"]):
            raise RuntimeError("invalid integer samples")
        if len(c["samples_ms"])!=r or any(abs(a/1e6-b)>1e-12 for a,b in zip(c["samples_ns"],c["samples_ms"])):
            raise RuntimeError("timer representations disagree")
        if abs(statistics.median(c["samples_ns"])/1e6-c["median_ms"])>1e-12:raise RuntimeError("stored median differs")
        if c["backend"].startswith(("all-","care-")):
            if c["simd_available"] is not True:raise RuntimeError("repair experiment requires active AVX2 dispatch")
            h=c["dirty_histogram"]
            if len(h)!=5 or any(type(x)!=int or x<0 for x in h):raise RuntimeError("histogram shape")
            if hist is None:hist=h
            if h!=hist:raise RuntimeError("candidate bounds/geometry differ")
            d=c["prefix_diagnostic"]
            if not d["available"]:raise RuntimeError("missing prefix diagnostic")
            if sum(h)!=d["vector_batches"]:raise RuntimeError("histogram coverage")
            if d["bounds_checked_rows"]!=case["rows"]:raise RuntimeError("bounds coverage")
            if d["certified_rows"]+d["ambiguous_rows"]!=case["rows"]:raise RuntimeError("bounds population mismatch")
            if d["fallback_batches"]!=sum(h[1:]):raise RuntimeError("dirty batch mismatch")
            if d["vector_rows"]!=256*sum(h):raise RuntimeError("vector extent mismatch")
            if d["dirty_words"]!=sum(k*h[k] for k in range(5))+d["scalar_fallback_batches"]:raise RuntimeError("dirty words/tail mismatch")
            if "expected_geometry" in case:
                e=case["expected_geometry"]
                if h!=e["histogram"] or d["ambiguous_rows"]!=e["ambiguous_rows"]:
                    raise RuntimeError("spatial geometry differs from independent raw-row audit")
                if d["dirty_words"]!=sum(k*h[k] for k in range(5)):raise RuntimeError("dirty word mismatch")
                policy=c["backend"].split("-",1)[1]
                units=sum(k*h[k] for k in range(5)) if policy=="word64" else (h[1]+4*sum(h[2:]) if policy=="hybrid" else 4*sum(h[1:]))
                if d["physical_refined_rows"]!=64*units or d["fallback_words"]!=units:
                    raise RuntimeError("derived physical repair count mismatch")
                if d["scalar_or_tail_rows"] or d["scalar_batches"]:raise RuntimeError("unexpected spatial scalar tail")
    if "expected_geometry" in case:
        if m["accepted"]!=case["rows"]-(case["expected_geometry"]["ambiguous_rows"]+1)//2:
            raise RuntimeError("spatial accepted count differs from direct arithmetic")
    return True

def audit_spatial(raw,case):
    """Read the actual row bytes; independently check declared placement and Q geometry."""
    rows=case["rows"]; pattern=case["pattern"]; h=[0]*5; words=[False]*4
    uncertain=0; counts=Counter(); seen=0
    for i,(a,b) in enumerate(struct.iter_unpack("<ii",raw.read_bytes())):
        if a!=b or a not in (1000,1001,1000000000):raise RuntimeError("unexpected spatial row")
        is_uncertain=a in (1000,1001)
        if pattern=="clear":expected=False
        elif pattern=="dense":expected=True
        elif pattern=="clustered":expected=i<rows//256
        elif pattern=="mixed":expected=(i%64==0 and ((i//256)%16)&(1<<((i%256)//64))!=0)
        else:expected=(i%64==0 and (i%256)//64 < {"one":1,"two":2,"three":3,"four":4}[pattern])
        if bool(expected)!=is_uncertain:raise RuntimeError("spatial placement differs")
        if is_uncertain:
            if a!=1000+uncertain%2:raise RuntimeError("spatial uncertain-value sequence differs")
            uncertain+=1;words[(i%256)//64]=True
        counts[a]+=1;seen+=1
        if i%256==255:h[sum(words)]+=1;words=[False]*4
    if seen!=rows:raise RuntimeError("spatial row count mismatch")
    return {"histogram":h,"ambiguous_rows":uncertain,"rows_checked":seen,
            "value_counts":dict(sorted(counts.items())),"raw_sha256":sha256(raw),
            "uncertainty_justification":"At prefix depth 12, 1000 and 1001 share the nonnegative interval [0,1048575], straddling >1000; 1000000000 is prefix-certified true."}

def validate_retained_runs(suite,output,complete=False):
    definitions=make_cases(suite["configuration"]["rows"])
    if suite["configuration"]["cases"]:
        chosen=set(suite["configuration"]["cases"]);definitions=[c for c in definitions if c["name"] in chosen]
    if len(suite["cases"])!=len(definitions) or [c["name"] for c in suite["cases"]]!=[c["name"] for c in definitions]:
        raise RuntimeError("case inventory changed")
    expected={(l,p) for l in range(2) for p in range(suite["configuration"]["processes"])}
    for case_index,(case,definition) in enumerate(zip(suite["cases"],definitions)):
        if any(case[k]!=v for k,v in definition.items()):raise RuntimeError("case definition changed")
        seen=set();identity=None
        for run in case["runs"]:
            job=(run["layout"],run["process"])
            if job not in expected or job in seen:raise RuntimeError("duplicate/invalid process job")
            expected_path=f"cases/{case['name']}/layout{job[0]}_process{job[1]}.json"
            if run["path"]!=expected_path:raise RuntimeError("measurement path does not identify its process job")
            seen.add(job);path=output/run["path"]
            if sha256(path)!=run["sha256"]:raise RuntimeError("retained measurement hash changed")
            m=json.loads(path.read_text());validate_measurement(m,case)
            if m["timing_order_seed"]!=0x5355464609000000+case_index*100+job[0]*10+job[1]:
                raise RuntimeError("measurement seed does not identify its process job")
            if m["minimum_repeat"]!=suite["configuration"]["repeat"] or m["repeat"]!=((suite["configuration"]["repeat"]+25)//26)*26:
                raise RuntimeError("round count differs from configuration")
            current=(m["accepted"],m["mask_checksum"],[(c["dirty_histogram"],c["prefix_diagnostic"]) for c in m["candidates"]])
            if identity is None:identity=current
            if current!=identity:raise RuntimeError("output/geometry differs across process runs")
        if complete and seen!=expected:raise RuntimeError("incomplete process/layout coverage")

def validate_inventory(suite,output):
    expected_families={c["family"] for c in suite["cases"]}
    if set(suite["families"])!=expected_families:raise RuntimeError("family inventory differs")
    for family,fr in suite["families"].items():
        for rel,h in fr["inventory"].items():
            if sha256(output/"families"/family/rel)!=h:raise RuntimeError("native artifact changed: "+rel)
        for variant in fr["variants"]:validate_emission(variant,variant["metadata"])
    for case in suite["cases"]:
        for suffix,h in case["input_sha256"].items():
            if sha256(output/"cases"/case["name"]/suffix)!=h:raise RuntimeError("input changed")

def summary(suite,output):
    validate_inventory(suite,output)
    validate_retained_runs(suite,output,complete=True)
    rows=[]
    for case in suite["cases"]:
        observations=[]
        for run in case["runs"]:
            m=json.loads((output/run["path"]).read_text())
            validate_measurement(m,case)
            med={c["backend"]:statistics.median(c["samples_ns"]) for c in m["candidates"]}
            entry={"layout":run["layout"],"process":run["process"],"medians_ns":med,
                   "ratios":{}}
            for sem in ("all","care"):
                for policy in ("word64","hybrid","duplicate"):
                    entry["ratios"][sem+"_"+policy]=med[sem+"-batch256"]/med[sem+"-"+policy]
            observations.append(entry)
        aggregates={}
        for name in observations[0]["ratios"]:
            vals=[r["ratios"][name] for r in observations]
            aggregates[name]={"geomean":math.exp(sum(map(math.log,vals))/len(vals)),
                              "min":min(vals),"max":max(vals),"each_process":vals}
        first=json.loads((output/case["runs"][0]["path"]).read_text())
        geometry={c["backend"]:{"histogram":c["dirty_histogram"],**c["prefix_diagnostic"]}
                  for c in first["candidates"] if c["backend"].startswith(("all-","care-"))}
        times={name:statistics.median(r["medians_ns"][name] for r in observations) for name in CANDIDATES}
        best_raw=min(("raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd"),key=times.get)
        rows.append({"case":case["name"],"family":case["family"],"observations":observations,
                     "aggregates":aggregates,"geometry":geometry,"median_of_process_medians_ns":times,
                     "best_raw_observed":best_raw,"raw_vs_care_hybrid":times[best_raw]/times["care-hybrid"]})
    process_count=suite["configuration"]["processes"]
    result={"schema_version":9,"functional_pilot":suite["configuration"]["pilot"],"scope":f"{process_count} fresh processes per each of two link orders; descriptive process-level ratios. Same host, same compiler, synthetic warm scans. No cross-host or production claim.",
            "cases":rows,"case_count":len(rows),"process_runs":sum(len(c["runs"]) for c in suite["cases"]),
            "full_mask_checks":sum(json.loads((output/r["path"]).read_text())["fresh_mask_checks"] for c in suite["cases"] for r in c["runs"])}
    write(output/"SUMMARY.json",result)
    lines=["# Suffice 0.9 repair granularity experiment","",
           f"Ratios are same-process Batch256 median / treatment median, geometrically averaged across {2*process_count} fresh process runs ({process_count} per link order). Values >1 favor the treatment. Min/max show individual process ratios. Functional pilot: {suite['configuration']['pilot']}.","",
           "| Case | All Word64 | All hybrid | Care Word64 | Care hybrid | Care duplicate |",
           "|---|---:|---:|---:|---:|---:|"]
    for c in rows:
        cols=[c["case"]]
        for k in ("all_word64","all_hybrid","care_word64","care_hybrid","care_duplicate"):
            v=c["aggregates"][k];cols.append(f'{v["geomean"]:.4f}× ({v["min"]:.3f}–{v["max"]:.3f})')
        lines.append("| "+" | ".join(cols)+" |")
    (output/"TABLES.md").write_text("\n".join(lines)+"\n")
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary",type=Path,default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--rows",type=int,default=1048576)
    p.add_argument("--repeat",type=int,default=26)
    p.add_argument("--processes",type=int,default=3)
    p.add_argument("--prepare-only",action="store_true")
    p.add_argument("--resume",action="store_true")
    p.add_argument("--functional-pilot",action="store_true")
    p.add_argument("--summary-only",action="store_true")
    p.add_argument("--cases",nargs="+")
    p.add_argument("--compiler")
    args=p.parse_args()
    if args.rows<512 or args.rows%512 or args.rows>16*1048576:p.error("rows must be 512-aligned within 512..16777216")
    if not 1<=args.repeat<=260 or not 1<=args.processes<=10:p.error("invalid repetition/process count")
    if hasattr(os,"sched_getaffinity"):
        os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    out=args.output.resolve();binary=args.binary.resolve()
    if args.summary_only:
        s=json.loads((out/"suite.json").read_text());print(json.dumps(summary(s,out)));return
    config=compiler_config(args.compiler)
    configuration={"rows":args.rows,"repeat":args.repeat,"processes":args.processes,
                   "pilot":args.functional_pilot,"cases":args.cases}
    if args.resume:
        suite=json.loads((out/"suite.json").read_text())
        if suite["configuration"]!=configuration:raise RuntimeError("resume configuration mismatch")
        if suite["sources"]!=source_identity():raise RuntimeError("source identity changed")
        if suite["environment"]!=environment():raise RuntimeError("environment changed; prepare a fresh experiment")
        if suite["compiler"]!=config or suite["binary_sha256"]!=sha256(binary):raise RuntimeError("compiler/emitter changed")
        validate_inventory(suite,out)
        validate_retained_runs(suite,out,complete=suite["status"]=="complete")
    else:
        out.mkdir(parents=True,exist_ok=False)
        cases=make_cases(args.rows)
        if args.cases:
            chosen=set(args.cases);cases=[c for c in cases if c["name"] in chosen]
            if len(cases)!=len(chosen):raise RuntimeError("unknown/duplicate case selection")
        suite={"schema_version":9,"configuration":configuration,"created_utc":now(),
               "sources":source_identity(),"environment":environment(),"compiler":config,
               "binary_sha256":sha256(binary),"families":{},"cases":cases,"status":"preparing"}
        write(out/"suite.json",suite)
        library,support=ensure_support(config);suite["support"]=support
        for family in dict.fromkeys(c["family"] for c in cases):
            suite["families"][family]=compile_family(binary,config,library,family,out/"families"/family)
            write(out/"suite.json",suite)
        for c in cases:
            d=out/"cases"/c["name"];d.mkdir(parents=True)
            raw=d/"input.i32";packed=d/"input.sfp";channels=PROGRAMS[c["family"]][0]
            t=time.perf_counter()
            if c["family"]=="spatial":c["generation"]=create_spatial(raw,c["rows"],c["pattern"])
            elif c["pattern"]=="prefix_stress":
                c["generation"]=prefix_stress(raw,c["rows"],channels,"magnitude_two",20261111)
            else:
                execute([binary,"generate","--output",raw,"--rows",c["rows"],"--channels",channels,
                         "--pattern",c["pattern"],"--seed",20261111],d/"generate.log")
            c["generate_ms"]=(time.perf_counter()-t)*1000
            c["pack_ms"]=execute([binary,"pack","--input",raw,"--output",packed,
                      "--channels",channels,"--block-rows",4096],d/"pack.log")
            c["input_sha256"]={x.name:sha256(x) for x in (raw,packed)};c["runs"]=[]
            if c["family"]=="spatial":c["expected_geometry"]=audit_spatial(raw,c)
        pair={c["name"]:c for c in cases}
        if "spatial_one" in pair and "spatial_clustered" in pair:
            if pair["spatial_one"]["expected_geometry"]["value_counts"]!=pair["spatial_clustered"]["expected_geometry"]["value_counts"]:
                raise RuntimeError("spatial pair multiset differs")
        if suite["sources"]!=source_identity():raise RuntimeError("source changed during preparation")
        suite["compilation_completed_utc"]=now();suite["status"]="prepared"
        write(out/"suite.json",suite)
    if args.prepare_only:
        print(json.dumps({"status":"prepared","families":len(suite["families"]),"cases":len(suite["cases"])}));return
    if suite["status"]=="complete":
        print(json.dumps({"status":"already complete","summary":str(out/"SUMMARY.json")}));return
    suite["measurement_started_utc"]=suite.get("measurement_started_utc",now())
    # Deterministically shuffle case/layout/process jobs to reduce a fixed drift order.
    jobs=[(i,l,r) for i in range(len(suite["cases"])) for l in range(2) for r in range(args.processes)]
    import random
    random.Random(2026111109).shuffle(jobs)
    for case_index,layout,process in jobs:
        case=suite["cases"][case_index]
        if any(r["layout"]==layout and r["process"]==process for r in case["runs"]):continue
        if suite["environment"]!=environment():raise RuntimeError("environment changed before resumed process")
        directory=out/"cases"/case["name"];dest=directory/f"layout{layout}_process{process}.json"
        if dest.exists():
            attempt=directory/"incomplete";attempt.mkdir(exist_ok=True)
            dest.replace(attempt/(dest.stem+"_"+str(time.time_ns())+".json"))
        family=suite["families"][case["family"]];exe=Path(family["layouts"][layout]["executable"])
        argv=[exe,directory/"input.i32",directory/"input.sfp",out/"families"/case["family"]/"program.suf",
              "0",args.repeat,dest,0x5355464609000000+case_index*100+layout*10+process]
        started=now();wall=execute(argv,directory/f"layout{layout}_process{process}.log")
        m=json.loads(dest.read_text());validate_measurement(m,case)
        case["runs"].append({"layout":layout,"process":process,"path":str(dest.relative_to(out)),
             "sha256":sha256(dest),"started_utc":started,"completed_utc":now(),"process_wall_ms":wall,
             "argv":list(map(str,argv))})
        write(out/"suite.json",suite)
    suite["measurement_completed_utc"]=now();suite["status"]="complete"
    if suite["sources"]!=source_identity():raise RuntimeError("source changed during measurement")
    write(out/"suite.json",suite);result=summary(suite,out)
    print(json.dumps({"status":"complete","cases":result["case_count"],"process_runs":result["process_runs"],
                      "full_mask_checks":result["full_mask_checks"]}))

if __name__=="__main__":
    main()

