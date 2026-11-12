#!/usr/bin/env python3
"""Same-process carry-state experiment with a frozen 0.5 generated control.

This reuses the retained full-mask/range/oracle driver, but deliberately excludes
the unrelated selector and portable-frontier races. All header emission and
compilation precede native execution. No observed timing chooses a strategy.
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
VARIANTS = (
    {"key":"frozen_residual", "namespace":"bench_frozen05_residual", "baseline":True,
     "flags":["--encoded-strategy","residual","--encoded-prefix-bits","12","--encoded-schedule","pressure"],
     "raw":False,"encoded":True,"label":"v05-residual-eager","prefix":12,"strategy":"residual","placement":"eager"},
    {"key":"eager", "namespace":"bench_eager06", "baseline":False,
     "flags":["--encoded-strategy","eager","--encoded-schedule","pressure"],
     "raw":True,"encoded":True,"label":"v06-eager","strategy":"eager","placement":"eager"},
    {"key":"soa", "namespace":"bench_soa06", "baseline":False,
     "flags":["--raw-layout","planar","--encoded-strategy","eager","--encoded-schedule","pressure"],
     "raw":True,"encoded":False,"label":"raw-soa","strategy":"eager","placement":"eager"},
    {"key":"frontier", "namespace":"bench_frontier06", "baseline":False,
     "flags":["--encoded-strategy","frontier","--encoded-prefix-bits","12","--encoded-schedule","pressure"],
     "raw":False,"encoded":True,"label":"v06-frontier","prefix":12,"strategy":"frontier","placement":"eager"},
    *({"key":key,"namespace":"bench_"+key+"06","baseline":False,
       "flags":["--encoded-strategy",strategy,"--encoded-prefix-bits","12","--encoded-schedule","pressure",
                "--encoded-carry-placement",placement],
       "raw":False,"encoded":True,"label":label,"prefix":12,"strategy":strategy,"placement":placement}
      for key,strategy,placement,label in (
          ("residual_eager","residual","eager","v06-residual-eager"),
          ("residual_guarded","residual","guarded","v06-residual-guarded"),
          ("care_eager","guarded-residual","eager","v06-care-eager"),
          ("care_guarded","guarded-residual","guarded","v06-care-guarded"))),
)
RAW = ("raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd")
GENERATED = tuple(v["label"] for v in VARIANTS if v["encoded"])
PREFIX = tuple(v["label"] for v in VARIANTS if "prefix" in v)
CANDIDATES = (*GENERATED,*RAW,"portable-wordcode")


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
    return ROOT/"baseline/v0_5/build"/("suffice.exe" if os.name=="nt" else "suffice")


def source_identity(root):
    paths = sorted((root/"src").glob("*.cpp")) + sorted((root/"include").rglob("*.hpp"))
    if not paths: raise RuntimeError("missing frozen 0.5 source tree")
    return {p.relative_to(root).as_posix():sha256(p) for p in paths}


def driver_source(declarations,registrations):
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
    source=prior.replace_once(source,'schema_version\\\":5','schema_version\\\":6')
    source=prior.replace_once(source,'0x5355464649434535ULL','0x5355464649434536ULL')
    source=prior.replace_once(source,'std::setprecision(12)','std::setprecision(std::numeric_limits<double>::max_digits10)')
    return source


def verify_native_artifacts(result,directory):
    if sha256(directory/"benchmark_interface.hpp")!=result["interface_sha256"]:
        raise RuntimeError("prepared native interface changed")
    for v in result["variants"]:
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
    if versions["baseline"]["version_output"]!="suffice 0.5.0" or versions["current"]["version_output"]!="suffice 0.6.0":
        raise RuntimeError("requires frozen 0.5 and current 0.6 executable identities")
    config=compiler_config(args.compiler)
    initial={"raw_sha256":sha256(args.raw),"packed_sha256":sha256(args.packed),"program":program,
             "program_sha256":sha256(program) if Path(program).is_file() else None}
    harness={str(Path(x).relative_to(ROOT)):sha256(x) for x in (__file__,prior.__file__,prior.previous.__file__,ROOT/"scripts/codegen_support.py")}
    frozen_sources=source_identity(ROOT/"baseline/v0_5")
    output.mkdir(parents=True)
    library,support=ensure_support(config,args.cache)
    (output/"benchmark_interface.hpp").write_text(prior.INTERFACE,encoding="utf-8")
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
        if "residual" in variant["strategy"] and not emitted.get("encoded_residual_emitted"):
            raise RuntimeError("requested residual silently fell back")
        if variant["strategy"]=="guarded-residual" and not emitted.get("encoded_guarded_residual_emitted"):
            raise RuntimeError("requested guarded residual silently fell back")
        if variant["strategy"]=="guarded-residual" and any(emitted.get(k)!=v for k,v in PROOF_BUDGETS.items()):
            raise RuntimeError("predeclared default proof budgets changed")
        if not variant["baseline"] and emitted.get("encoded_carry_placement")!=variant["placement"]:
            raise RuntimeError("emitted carry placement differs from request")
        source=output/f"wrapper_{key}.cpp";source.write_text(prior.wrapper_source(variant),encoding="utf-8")
        obj=output/(f"wrapper_{key}"+(".obj" if config["msvc"] else ".o"))
        compiled=prior.compile_object(config,source,obj,output,output/f"compile_{key}.log")
        objects.append(obj)
        variants.append({**variant,"header":header.name,"header_sha256":sha256(header),"header_metadata":header_metadata(header),
                         "emission_metadata_sha256":sha256(metadata),"emission_command":command,
                         "emission_process_wall_ms":emission_ms,"emission":emitted,"compilation":compiled})
    declarations="\n".join(f'void register_{v["key"]}(std::vector<Candidate>&,Native,Native,std::uint64_t,std::uint32_t,const suffice::PackedDataset&);' for v in VARIANTS)
    registrations="\n".join(f'        register_{v["key"]}(candidates,aos.span(),soa.span(),rows,channels,packed);' for v in VARIANTS)
    source=output/"driver.cpp";source.write_text(driver_source(declarations,registrations),encoding="utf-8")
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
    if source_identity(ROOT/"baseline/v0_5")!=frozen_sources:raise RuntimeError("frozen source changed")
    result={"schema_version":6,"status":"prepared" if args.prepare_only else "completed",
            "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "environment":{"platform":platform.platform(),"python":platform.python_version(),"available_affinity_before":available,"benchmark_affinity":affinity},
            "binaries":versions,"compiler":config,"inputs":initial,"harness_sources":harness,"fixed_prefix_bits":DEPTH,
            "fixed_proof_budgets":PROOF_BUDGETS,
            "variants":variants,"shared_support_library":support,"frozen_sources":frozen_sources,
            "driver_compilation":compiled,"interface_sha256":sha256(output/"benchmark_interface.hpp"),
            "link":{"argv":link,"wall_ms":link_ms,"binary_sha256":sha256(executable)},
            "execution":{"argv":command,"process_wall_ms":execution_ms},"measurements":measurements,
            "scope":[
                "Twelve candidates coexist in one process; separate wrapper translation units; no requested LTO.",
                "Frozen control is emitted by the preserved 0.5 CLI and compiled by the same current host compiler. It uses the shared unchanged packed reader interface, not a separate full 0.5 process.",
                "Every fresh complete mask and each arbitrary range mask is checked against the original scalar arithmetic IR outside timing.",
                "Current residual eager/guarded and care eager/guarded form a two-by-two experiment isolating state elimination and materialization placement.",
                "Raw AoS/SoA scalar/SIMD and current eager/wordcode/frontier remain explicit controls. Copy/transpose/validation/compile costs are separate.",
                "Generated prefix diagnostics reconstruct 64/256-row branch geometry outside timing. They do not measure hardware traffic or stack high-water marks.",
                "Compiler/generator wall times are observed; isolated compiler or proof-engine peak memory is not measured.",
                "Fixed depth12, default proof budgets, and pressure schedule are predeclared. No timing result installs a planner policy.",
                "These warm synthetic results do not establish Windows, application-wide, arbitrary-AI, external-engine, or hardware-optimal gains."
            ]}
    prior.previous.write_json(output/"result.json",result)
    print(json.dumps({"output":str(output),"status":result["status"],"candidates":len(CANDIDATES),
                      "exact":measurements["exact"] if measurements else None},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError,subprocess.CalledProcessError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
