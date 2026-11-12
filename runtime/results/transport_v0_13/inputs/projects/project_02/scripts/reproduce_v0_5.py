#!/usr/bin/env python3
"""Prepare all focused v0.5 cases, then measure with no overlapping compilation.

The namespace-renamed frozen0.4 and current native implementations share each
comparison process. Selector policy/frozen-CLI runs are separate processes on
identical inputs, with fresh within-process controls and separately reported
regret. No observed timing chooses a prefix depth or the case definitions.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import sys

from benchmark_v0_2 import PRESETS, digest, write_json
from benchmark_v0_5 import default_baseline, source_identity
from codegen_support import ROOT
from reproduce_v0_3 import capture_provenance, run_command
from reproduce_v0_4 import prefix_stress, spatial_stress

POLICIES = ("current-representative", "current-legacy-endpoints", "frozen04-legacy-endpoints")
SEED_SUBSET = {"magnitude_two_uniform", "magnitude_two_prefix_stress", "overlapping_gates_uniform",
               "magnitude_spatial_scattered", "magnitude_spatial_clustered"}


def case_definitions(rows):
    result=[]
    def add(family,channels,program,patterns,threshold=None,count=None,constant=False):
        for pattern in patterns:
            suffix=pattern.removeprefix("spatial_") if family=="magnitude_spatial" else pattern
            result.append({"name":family+"_"+suffix,"family":family,"pattern":pattern,"rows":count or rows,
                           "channels":channels,"block_rows":4096,"program":program,"threshold":threshold,
                           "constant_control":constant})
    add("sign",1,"identity",["uniform"],-1)
    add("gate_eight",8,"examples/predicate_eight_channel_gate.suf",["heterogeneous"])
    add("magnitude_two",2,"preset:magnitude-intersection",["uniform","heterogeneous"])
    add("magnitude_two",2,"preset:magnitude-intersection",["prefix_stress"],count=rows+17)
    add("mixed_three",3,"preset:mixed-three-channels",["uniform"])
    add("overlapping_gates",2,"examples/predicate_overlapping_gates.suf",["uniform","prefix_stress"])
    add("magnitude_spatial",2,"spatial:magnitude1000",["spatial_scattered","spatial_clustered"])
    add("magnitude_constant",2,"preset:magnitude-intersection",["boundary"],constant=True)
    return result


def identities(binary,baseline):
    return {"current":{"path":str(binary),"sha256":digest(binary)},
            "baseline":{"path":str(baseline),"sha256":digest(baseline)}}


def verify_sources(binary,baseline,binaries,provenance,frozen_sources):
    if identities(binary,baseline)!=binaries:raise RuntimeError("an executable changed during matrix")
    if capture_provenance(True)!=provenance:raise RuntimeError("source/compiler identity changed during matrix")
    if source_identity(ROOT/"baseline/v0_4")!=frozen_sources:raise RuntimeError("frozen source identity changed")


def verify_native_artifacts(result,directory):
    # Retained object hashes protect prepared/restarted runs, as do full wrapper,
    # interface, driver and final executable identities.
    if digest(directory/"benchmark_interface.hpp")!=result["interface_sha256"]:
        raise RuntimeError("prepared interface changed")
    for variant in result["variants"]:
        if digest(directory/variant["header"])!=variant["header_sha256"]:raise RuntimeError("prepared generated header changed")
        if digest(directory/f'wrapper_{variant["key"]}.cpp')!=variant["compilation"]["source_sha256"]:
            raise RuntimeError("prepared generated wrapper changed")
        obj=directory/(f'wrapper_{variant["key"]}.obj' if result["compiler"]["msvc"] else f'wrapper_{variant["key"]}.o')
        if digest(obj)!=variant["compilation"]["object_sha256"]:raise RuntimeError("prepared generated object changed")
    for key,stem in (("driver_compilation","driver"),("frozen_portable_compilation","wrapper_frozen_portable")):
        if digest(directory/(stem+".cpp"))!=result[key]["source_sha256"]:raise RuntimeError("prepared driver/frozen wrapper changed")
        if digest(directory/(stem+(".obj" if result["compiler"]["msvc"] else ".o")))!=result[key]["object_sha256"]:
            raise RuntimeError("prepared driver/frozen object changed")
    executable=directory/("comparison.exe" if result["compiler"]["msvc"] else "comparison")
    if digest(executable)!=result["link"]["binary_sha256"]:raise RuntimeError("prepared native executable changed")


def create_case(binary,baseline,definition,directory,seed,repeat,compiler):
    directory.mkdir()
    case={**definition,"seed":seed,"status":"preparing"}
    raw,packed=directory/"input.i32",directory/"input.sfp"
    if definition["pattern"].startswith("spatial_"):
        case["generation"]=spatial_stress(raw,definition["rows"],definition["pattern"])
        write_json(directory/"generate.json",case["generation"])
    elif definition["pattern"]=="prefix_stress":
        case["generation"]=prefix_stress(raw,definition["rows"],definition["channels"],definition["family"],seed)
        write_json(directory/"generate.json",case["generation"])
    else:
        case["generation"]=run_command([binary,"generate","--output",raw,"--rows",definition["rows"],"--channels",definition["channels"],
                                         "--pattern",definition["pattern"],"--seed",seed],directory/"generate.log")
    case["packing"]=run_command([binary,"pack","--input",raw,"--output",packed,"--channels",definition["channels"],
                                 "--block-rows",definition["block_rows"]],directory/"pack.log")
    case["raw_sha256"],case["packed_sha256"]=digest(raw),digest(packed)
    program=definition["program"]
    if program=="spatial:magnitude1000":
        path=directory/"program.suf"
        path.write_text("input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 1000\n",encoding="utf-8")
        program=str(path)
    elif program.startswith("preset:"):
        path=directory/"program.suf";path.write_text(PRESETS[program.split(":",1)[1]][1],encoding="utf-8");program=str(path)
    elif program.endswith(".suf"):
        path=directory/"program.suf";shutil.copy2(ROOT/program,path);program=str(path)
    case["program_used"]=program;case["program_sha256"]=digest(Path(program)) if Path(program).is_file() else None
    command=[sys.executable,ROOT/"scripts/benchmark_v0_5.py","--binary",binary,"--baseline-binary",baseline,
             "--raw",raw,"--packed",packed,"--program",program,"--output",directory/"native","--repeat",repeat,
             "--compiler",compiler,"--pin-cpu","none","--prepare-only"]
    if definition["threshold"] is not None:command += ["--threshold",definition["threshold"]]
    case["native_preparation"]=run_command(command,directory/"native_prepare.log",timeout=1800)
    case["native_result"]=json.loads((directory/"native/result.json").read_text(encoding="utf-8"))
    if case["native_result"]["status"]!="prepared":raise RuntimeError("preparation unexpectedly executed measurements")
    case["status"]="prepared"
    write_json(directory/"case.json",case)
    return case


def measure_case(case,directory,binary,baseline,core_repeat,seeds,skip_selection,case_index):
    raw,packed=directory/"input.i32",directory/"input.sfp"
    if digest(raw)!=case["raw_sha256"] or digest(packed)!=case["packed_sha256"]:
        raise RuntimeError("prepared input changed")
    native=directory/"native";result=case["native_result"]
    verify_native_artifacts(result,native)
    print("  same-process frozen/current native comparisons",flush=True)
    case["native_execution"]=run_command(result["execution"]["argv"],directory/"native_execute.log",timeout=1800)
    result["execution"]["process_wall_ms"]=case["native_execution"]["process_wall_ms"]
    result["measurements"]=json.loads((native/"measurements.json").read_text(encoding="utf-8"))
    result["status"]="completed";result["measured_utc"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
    if not result["measurements"]["exact"] or len(result["measurements"]["candidates"])!=14:
        raise RuntimeError("native comparison failed exactness or candidate count")
    verify_native_artifacts(result,native)
    write_json(native/"result.json",result)
    case["selection_runs"]=[]
    if not skip_selection:
        used_seeds=seeds if case["name"] in SEED_SUBSET else seeds[:1]
        for seed_index,seed in enumerate(used_seeds):
            # Across successive cases/seed indices use the six Williams rows.
            # Cross-process order is retained, but no between-process timing
            # ratio is claimed to inherit the within-process balance guarantee.
            indices=((0,1,2),(1,2,0),(2,0,1),(2,1,0),(0,2,1),(1,0,2))[(case_index+seed_index)%6]
            for position,index in enumerate(indices):
                policy=POLICIES[index];frozen=policy.startswith("frozen")
                executable=baseline if frozen else binary
                stem=f"selection_{seed}_{policy}"
                command=[executable,"compare","--input",raw,"--packed",packed,"--program",case["program_used"],
                         "--repeat",core_repeat,"--calibration","sampled","--calibration-repeat",3,
                         "--sample-seed",seed,"--json",directory/(stem+".json")]
                if not frozen:command += ["--sample-policy","representative" if policy=="current-representative" else "legacy-endpoints"]
                if case["threshold"] is not None:command += ["--threshold",case["threshold"]]
                print(f"  {policy}; seed {seed}",flush=True)
                execution=run_command(command,directory/(stem+".log"),timeout=1800)
                core=json.loads((directory/(stem+".json")).read_text(encoding="utf-8"))
                truth=result["measurements"]
                check=core.get("verification",{})
                if not check.get("exact_match") or check["mask_checksum"]!=truth["mask_checksum"] or check["accepted"]!=truth["accepted"]:
                    raise RuntimeError("selector/core/native exact outputs disagree")
                selection=core["selection"]
                if selection["total_calibration_row_evaluations"]>selection["calibration_row_budget"]:
                    raise RuntimeError("sampled policy exceeded requested row budget")
                if core["fresh_execution"]["partial_cycle_rounds"]:
                    raise RuntimeError("selector fresh comparison did not complete balanced cycles")
                case["selection_runs"].append({"policy":policy,"seed":seed,"process_order_position":position,
                                                "execution":execution,"result":core})
    if digest(raw)!=case["raw_sha256"] or digest(packed)!=case["packed_sha256"]:
        raise RuntimeError("input changed during measurements")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    parser.add_argument("--baseline-binary",type=Path,default=default_baseline())
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--rows",type=int,default=1048576)
    parser.add_argument("--repeat",type=int,default=40)
    parser.add_argument("--core-repeat",type=int,default=36,help="Rounded up to complete18round cycles for eight fixed candidates plus adaptive")
    parser.add_argument("--seed",type=int,default=20261111)
    parser.add_argument("--selection-seeds",type=int,nargs="+",help="Default generator seed, seed+1, seed+2; extra seeds used on predeclared fivecase subset")
    parser.add_argument("--cases",nargs="+")
    parser.add_argument("--pin-cpu",default="auto")
    parser.add_argument("--skip-selection","--skip-core",dest="skip_selection",action="store_true")
    parser.add_argument("--keep-data",action="store_true")
    parser.add_argument("--prepare-only",action="store_true",help="Build all case artifacts without executing performance measurements")
    parser.add_argument("--resume",action="store_true")
    args=parser.parse_args()
    seeds=args.selection_seeds or [args.seed,args.seed+1,args.seed+2]
    if args.rows<1 or not 1<=args.repeat<=1000 or not 1<=args.core_repeat<=990:
        parser.error("invalid row/repetition counts")
    if any(not 0<=seed<(1<<64) for seed in [args.seed,*seeds]) or len(set(seeds))!=len(seeds):
        parser.error("seeds must be distinct unsigned64 values")
    core_repeat=((args.core_repeat+17)//18)*18
    binary,baseline=args.binary.resolve(),args.baseline_binary.resolve()
    if not binary.is_file() or not baseline.is_file():parser.error("build current0.5 and frozen0.4 executables first")
    available=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    if args.pin_cpu!="none" and available:
        cpu=available[0] if args.pin_cpu=="auto" else int(args.pin_cpu)
        if cpu not in available:parser.error("CPU outside allowed affinity")
        os.sched_setaffinity(0,{cpu})
    affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    definitions=case_definitions(args.rows)
    if args.cases:
        requested=set(args.cases);definitions=[c for c in definitions if c["name"] in requested or c["family"] in requested]
        if not definitions:parser.error("no matching case")
    if any(c["family"]=="magnitude_spatial" for c in definitions) and (args.rows<512 or args.rows%512):
        parser.error("spatial pair requires rows divisible by512 and at least512")
    output=args.output.resolve();suite_path=output/"suite.json"
    configuration={"rows":args.rows,"minimum_repeat":args.repeat,"core_repeat":core_repeat,"seed":args.seed,
                   "selection_seeds":seeds,"multi_seed_subset":sorted(SEED_SUBSET),"skip_selection":args.skip_selection,
                   "definitions":definitions,"affinity":affinity,"fixed_prefix_bits":12,"dense_threshold_percent":25,
                   "compile_then_measure":True}
    provenance=capture_provenance(True);binaries=identities(binary,baseline);frozen_sources=source_identity(ROOT/"baseline/v0_4")
    if output.exists():
        if not args.resume or not suite_path.is_file():parser.error("use fresh output or explicit valid --resume")
        suite=json.loads(suite_path.read_text(encoding="utf-8"))
        for key,value in (("configuration",configuration),("binaries",binaries),("provenance",provenance),("frozen_sources",frozen_sources)):
            if suite.get(key)!=value:parser.error("resume refused: changed "+key)
        if any(c["status"] not in {"prepared","completed"} for c in suite["cases"]):
            parser.error("partial case needs explicit inspection before resume")
    else:
        output.mkdir(parents=True)
        suite={"schema_version":5,"status":"preparing","created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "configuration":configuration,"binaries":binaries,"provenance":provenance,"frozen_sources":frozen_sources,
               "environment":{"platform":platform.platform(),"python":platform.python_version(),"machine":platform.machine(),
                              "available_affinity":available,"benchmark_affinity":affinity},"cases":[]}
    write_json(suite_path,suite)
    try:
        by_name={case["name"]:case for case in suite["cases"]}
        for number,definition in enumerate(definitions,1):
            verify_sources(binary,baseline,binaries,provenance,frozen_sources)
            if definition["name"] in by_name:continue
            directory=output/definition["name"]
            if directory.exists():raise RuntimeError("partial case directory needs explicit inspection: "+str(directory))
            print(f'PREPARE [{number}/{len(definitions)}] {definition["name"]}',flush=True)
            case=create_case(binary,baseline,definition,directory,args.seed,args.repeat,provenance["native_compiler"]["executable"])
            suite["cases"].append(case);by_name[case["name"]]=case;write_json(suite_path,suite)
        suite["status"]="prepared";suite["all_compilation_completed_utc"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
        write_json(suite_path,suite)
        if args.prepare_only:
            print(json.dumps({"status":"prepared","matrix":str(output),"cases":len(suite["cases"])},indent=2));return
        suite["status"]="measuring";write_json(suite_path,suite)
        for number,definition in enumerate(definitions,1):
            case=by_name[definition["name"]]
            if case["status"]=="completed":continue
            verify_sources(binary,baseline,binaries,provenance,frozen_sources)
            print(f'MEASURE [{number}/{len(definitions)}] {case["name"]}',flush=True)
            directory=output/case["name"]
            measure_case(case,directory,binary,baseline,core_repeat,seeds,args.skip_selection,number-1)
            verify_sources(binary,baseline,binaries,provenance,frozen_sources)
            case["data_retained"]=args.keep_data
            if not args.keep_data:
                (directory/"input.i32").unlink();(directory/"input.sfp").unlink()
            case["status"]="completed";write_json(directory/"case.json",case);write_json(suite_path,suite)
        suite["status"]="completed";suite["completed_utc"]=datetime.datetime.now(datetime.timezone.utc).isoformat()
    except Exception as error:
        suite["status"]="failed";suite["failure"]=str(error);write_json(suite_path,suite);raise
    write_json(suite_path,suite)
    print(json.dumps({"status":suite["status"],"cases":len(suite["cases"]),"matrix":str(output)},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
