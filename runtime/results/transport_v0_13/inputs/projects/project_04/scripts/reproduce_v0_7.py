#!/usr/bin/env python3
"""Prepare the fixed scheduling/rematerialization and portfolio matrix completely before any measurements.

Resume preserves the original compilation-completion timestamp. Incomplete
measurement outputs are archived before the whole case is repeated; completed
cases are never retimed by resume. Source/input/object identities remain fixed.
"""
from __future__ import annotations

import argparse
from collections import Counter
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import sys

from benchmark_v0_2 import PRESETS, digest
from benchmark_v0_7 import (CALIBRATION, CANDIDATES, PORTFOLIO_ELIGIBLE, PROOF_BUDGETS,
                            default_baseline, frozen_release_identity, source_identity, verify_native_artifacts)
from codegen_support import ROOT
from reproduce_v0_3 import capture_provenance, run_command
from reproduce_v0_4 import prefix_stress, spatial_stress


def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path,value):
    path=Path(path);temporary=path.with_name(path.name+".tmp")
    temporary.write_text(json.dumps(value,indent=2)+"\n",encoding="utf-8")
    temporary.replace(path)


def case_definitions(rows):
    cases=[]
    def add(name,family,pattern,program,channels=2,extra=0,category="ordinary"):
        cases.append({"name":name,"family":family,"pattern":pattern,"program":program,
                      "channels":channels,"rows":rows+extra,"block_rows":4096,"category":category})
    add("magnitude_two_uniform","magnitude_two","uniform","preset:magnitude-intersection")
    add("magnitude_two_heterogeneous","magnitude_two","heterogeneous","preset:magnitude-intersection")
    add("overlapping_gates_uniform","overlapping_gates","uniform","examples/predicate_overlapping_gates.suf")
    add("magnitude_two_prefix_stress","magnitude_two","prefix_stress","preset:magnitude-intersection",extra=17,category="stress")
    add("magnitude_spatial_scattered","magnitude_spatial","spatial_scattered","spatial:magnitude1000",category="spatial")
    add("magnitude_spatial_clustered","magnitude_spatial","spatial_clustered","spatial:magnitude1000",category="spatial")
    add("pure_and_four_uniform","pure_and_four","uniform","care:and4",channels=4,category="structural")
    add("pure_or_four_uniform","pure_or_four","uniform","care:or4",channels=4,category="structural")
    add("shared_diamond_uniform","shared_diamond","uniform","care:diamond",channels=3,category="structural")
    return cases


def identities(binary,baseline):
    return {"current":{"path":str(binary),"sha256":digest(binary)},
            "baseline":{"path":str(baseline),"sha256":digest(baseline)}}


def verify_sources(binary,baseline,suite):
    if identities(binary,baseline)!=suite["binaries"]:raise RuntimeError("an executable changed during matrix")
    if capture_provenance(True)!=suite["provenance"]:raise RuntimeError("source/compiler identity changed during matrix")
    if source_identity(ROOT/"baseline/v0_6")!=suite["frozen_sources"]:raise RuntimeError("frozen sources changed")
    if frozen_release_identity()!=suite["frozen_release"]:raise RuntimeError("full frozen release archive changed")


def stash_directory(directory,parent):
    parent.mkdir(parents=True,exist_ok=True)
    number=1
    while (parent/f"{directory.name}_attempt_{number:03}").exists():number+=1
    destination=parent/f"{directory.name}_attempt_{number:03}"
    directory.replace(destination)
    return str(destination)


def create_case(binary,baseline,definition,directory,seed,repeat,compiler,functional_pilot=False):
    directory.mkdir()
    case={**definition,"seed":seed,"status":"preparing","preparation_started_utc":now()}
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
        content="input a 0 0\ninput b 1 0\nabs aa a\nabs bb b\nmin both aa bb\noutput both > 1000\n"
    elif program.startswith("preset:"):
        content=PRESETS[program.split(":",1)[1]][1]
    elif program=="care:diamond":
        content="input a 0 0\ninput b 1 0\ninput c 2 0\nmin ab a b\nmin ac a c\nmax root ab ac\noutput root > 100000000\n"
    elif program.startswith("care:"):
        operation="min" if program=="care:and4" else "max"
        content="".join(f"input a{i} {i} 0\n" for i in range(4))
        content+=f"{operation} p0 a0 a1\n{operation} p1 a2 a3\n{operation} root p0 p1\noutput root > 100000000\n"
    else:
        content=(ROOT/program).read_text(encoding="utf-8")
    path=directory/"program.suf";path.write_text(content,encoding="utf-8")
    case["program_used"],case["program_sha256"]=str(path),digest(path)
    command=[sys.executable,ROOT/"scripts/benchmark_v0_7.py","--binary",binary,"--baseline-binary",baseline,
             "--raw",raw,"--packed",packed,"--program",path,"--output",directory/"native",
             "--repeat",repeat,"--compiler",compiler,"--pin-cpu","none","--prepare-only"]
    if functional_pilot:command.append("--functional-pilot")
    case["native_preparation"]=run_command(command,directory/"native_prepare.log",timeout=3600)
    case["native_result"]=json.loads((directory/"native/result.json").read_text(encoding="utf-8"))
    if case["native_result"]["status"]!="prepared":raise RuntimeError("preparation executed performance measurements")
    if digest(raw)!=case["raw_sha256"] or digest(packed)!=case["packed_sha256"]:raise RuntimeError("input changed during preparation")
    case["status"]="prepared";case["preparation_completed_utc"]=now()
    write_json(directory/"case.json",case)
    return case


def spatial_input_audit(cases,output):
    selected={c["pattern"]:c for c in cases if c["category"]=="spatial"}
    if set(selected)!={"spatial_scattered","spatial_clustered"}:return None
    records={}
    for pattern,case in selected.items():
        path=output/case["name"]/"input.i32"
        if not path.is_file():raise RuntimeError("spatial audit needs the prepared raw inputs")
        if digest(path)!=case["raw_sha256"]:raise RuntimeError("spatial audit input hash changed")
        counts=Counter();rows=case["rows"];uncertain=rows//256;seen=0
        for index,pair in enumerate(struct.iter_unpack("<ii",path.read_bytes())):
            expected_unknown=index%256==0 if pattern=="spatial_scattered" else index<uncertain
            uncertainty_index=index//256 if pattern=="spatial_scattered" else index
            value=1000+uncertainty_index%2 if expected_unknown else 1000000000
            if pair!=(value,value):raise RuntimeError("spatial row arrangement differs from declared construction")
            counts[pair]+=1;seen+=1
        if seen!=rows:raise RuntimeError("spatial row count changed")
        expected={(1000,1000):uncertain//2,(1001,1001):uncertain//2,(1000000000,1000000000):rows-uncertain}
        if counts!=Counter(expected):raise RuntimeError("spatial actual row multiset differs")
        records[pattern]={"case":case["name"],"raw_sha256":case["raw_sha256"],"positions_checked":seen,
                          "counts":[{"pair":list(pair),"rows":count} for pair,count in sorted(counts.items())]}
    if records["spatial_scattered"]["counts"]!=records["spatial_clustered"]["counts"]:raise RuntimeError("spatial multisets differ")
    record={"created_utc":now(),"actual_raw_rows_checked":True,"row_multisets_equal":True,"cases":records,
            "scope":"Separate explicit read of both actual raw files; no native generator output or emitted kernel serves as the row oracle."}
    write_json(output/"spatial_input_audit.json",record)
    return record


def preserve_incomplete_measurement(case,directory):
    paths=[directory/"native/measurements.json",directory/"native/execution.log",directory/"native_execute.log"]
    existing=[p for p in paths if p.exists()]
    if not existing:return
    parent=directory/"discarded_attempts";parent.mkdir(exist_ok=True)
    number=1
    while (parent/f"attempt_{number:03}").exists():number+=1
    destination=parent/f"attempt_{number:03}";destination.mkdir()
    for path in existing:
        name="native_"+path.name if path.parent.name=="native" else path.name
        path.replace(destination/name)
    shutil.copy2(directory/"native/result.json",destination/"native_result.json")
    event={"recorded_utc":now(),"directory":str(destination),"reason":"Incomplete case is discarded and wholly remeasured with unchanged inputs/artifacts."}
    case.setdefault("discarded_measurements",[]).append(event)
    write_json(destination/"status.json",event)


def clean_completed_inputs(case,directory,keep):
    if keep:return
    for name in ("input.i32","input.sfp"):
        path=directory/name
        if path.exists():path.unlink()
    case["data_retained"]=False


def measure_case(case,directory):
    raw,packed=directory/"input.i32",directory/"input.sfp"
    if digest(raw)!=case["raw_sha256"] or digest(packed)!=case["packed_sha256"]:raise RuntimeError("prepared input changed")
    if digest(directory/"program.suf")!=case["program_sha256"]:raise RuntimeError("prepared program changed")
    result=case["native_result"];verify_native_artifacts(result,directory/"native")
    case["measurement_started_utc"]=now()
    case["native_execution"]=run_command(result["execution"]["argv"],directory/"native_execute.log",timeout=1800)
    result["execution"]["process_wall_ms"]=case["native_execution"]["process_wall_ms"]
    result["measurements"]=json.loads((directory/"native/measurements.json").read_text(encoding="utf-8"))
    if not result["measurements"]["exact"] or {c["backend"] for c in result["measurements"]["candidates"]}!=set(CANDIDATES):
        raise RuntimeError("native exactness or candidate inventory failed")
    result["status"]="completed";result["measured_utc"]=now()
    verify_native_artifacts(result,directory/"native")
    if digest(raw)!=case["raw_sha256"] or digest(packed)!=case["packed_sha256"]:raise RuntimeError("input changed during measurement")
    if digest(directory/"program.suf")!=case["program_sha256"]:raise RuntimeError("program changed during measurement")
    write_json(directory/"native/result.json",result)
    case["status"]="completed";case["measurement_completed_utc"]=now();case["data_retained"]=True


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,default=ROOT/"build"/("suffice.exe" if os.name=="nt" else "suffice"))
    parser.add_argument("--baseline-binary",type=Path,default=default_baseline());parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--rows",type=int,default=1048576);parser.add_argument("--repeat",type=int,default=36)
    parser.add_argument("--seed",type=int,default=20261111);parser.add_argument("--cases",nargs="+")
    parser.add_argument("--pin-cpu",default="auto");parser.add_argument("--keep-data",action="store_true")
    parser.add_argument("--prepare-only",action="store_true");parser.add_argument("--resume",action="store_true")
    parser.add_argument("--functional-pilot",action="store_true",help="Mark all retained observations as non-performance pipeline evidence")
    args=parser.parse_args()
    if args.rows<1 or not 1<=args.repeat<=1000 or not 0<=args.seed<1<<64:parser.error("invalid row/repetition/seed count")
    binary,baseline=args.binary.resolve(),args.baseline_binary.resolve()
    if not binary.is_file() or not baseline.is_file():parser.error("build current0.7 and frozen0.6 executables first")
    available=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    if args.pin_cpu!="none" and available:
        cpu=available[0] if args.pin_cpu=="auto" else int(args.pin_cpu)
        if cpu not in available:parser.error("CPU outside permitted affinity")
        os.sched_setaffinity(0,{cpu})
    affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None
    definitions=case_definitions(args.rows)
    if args.cases:
        wanted=set(args.cases);definitions=[c for c in definitions if c["name"] in wanted or c["family"] in wanted]
        if not definitions:parser.error("no matching case")
    if any(c["category"]=="spatial" for c in definitions) and (args.rows<512 or args.rows%512):
        parser.error("spatial cases require rows divisible by512 and at least512")
    output=args.output.resolve();suite_path=output/"suite.json"
    configuration={"measurement_purpose":"functional_pipeline_pilot" if args.functional_pilot else "fixed_performance_experiment",
                   "rows":args.rows,"minimum_repeat":args.repeat,"seed":args.seed,"definitions":definitions,
                   "affinity":affinity,"fixed_prefix_bits":12,"carry_placements":["guarded","recompute"],"schedules":["pressure","bounds-first"],
                   "proof_budgets":PROOF_BUDGETS,"compile_then_measure":True,"keep_data":args.keep_data,
                   "objective":"Isolate physical scheduling and cold carry recomputation; evaluate actual bounded portfolio selection in fresh same-input rounds; retain every predeclared case and all negative outcomes.",
                   "portfolio_calibration":CALIBRATION,"portfolio_eligible_controls":list(PORTFOLIO_ELIGIBLE),
                   "candidate_inventory":list(CANDIDATES)}
    provenance=capture_provenance(True);binaries=identities(binary,baseline);frozen=source_identity(ROOT/"baseline/v0_6");frozen_release=frozen_release_identity()
    if output.exists():
        if not args.resume or not suite_path.is_file():parser.error("use fresh output or explicit valid --resume")
        suite=json.loads(suite_path.read_text(encoding="utf-8"))
        for key,value in (("configuration",configuration),("binaries",binaries),("provenance",provenance),("frozen_sources",frozen),("frozen_release",frozen_release)):
            if suite.get(key)!=value:parser.error("resume refused: changed "+key)
        if any(c["status"] not in {"preparing","prepared","measuring","completed"} for c in suite["cases"]):parser.error("unknown case state")
        if "failure" in suite:suite.setdefault("prior_failures",[]).append(suite.pop("failure"))
        suite.setdefault("resumed_utc",[]).append(now())
    else:
        output.mkdir(parents=True)
        suite={"schema_version":7,"status":"preparing","created_utc":now(),"configuration":configuration,
               "objective_and_workload_frozen_utc":now(),
               "binaries":binaries,"provenance":provenance,"frozen_sources":frozen,"frozen_release":frozen_release,
               "environment":{"platform":platform.platform(),"python":platform.python_version(),"machine":platform.machine(),
                              "available_affinity":available,"benchmark_affinity":affinity},"cases":[]}
    write_json(suite_path,suite)
    try:
        by_name={c["name"]:c for c in suite["cases"]}
        for number,definition in enumerate(definitions,1):
            verify_sources(binary,baseline,suite)
            case=by_name.get(definition["name"]);directory=output/definition["name"]
            if case and case["status"]!="preparing":continue
            if directory.exists():
                preserved=stash_directory(directory,output/"discarded_preparation")
                suite.setdefault("discarded_preparations",[]).append({"case":definition["name"],"directory":preserved,"recorded_utc":now()})
            if case is None:
                case={**definition,"status":"preparing"};suite["cases"].append(case);by_name[definition["name"]]=case
            suite["status"]="preparing";write_json(suite_path,suite)
            print(f'PREPARE [{number}/{len(definitions)}] {definition["name"]}',flush=True)
            prepared=create_case(binary,baseline,definition,directory,args.seed,args.repeat,provenance["native_compiler"]["executable"],args.functional_pilot)
            case.clear();case.update(prepared);write_json(suite_path,suite)
        verify_sources(binary,baseline,suite)
        for case in suite["cases"]:verify_native_artifacts(case["native_result"],output/case["name"]/"native")
        if "all_compilation_completed_utc" not in suite:
            suite["all_compilation_completed_utc"]=now()
        suite["last_preflight_completed_utc"]=now()
        if not (output/"spatial_input_audit.json").exists():
            spatial_input_audit(suite["cases"],output)
        suite["status"]="prepared";write_json(suite_path,suite)
        if args.prepare_only:
            print(json.dumps({"status":"prepared","cases":len(suite["cases"]),"matrix":str(output)},indent=2));return
        suite["status"]="measuring";suite.setdefault("measurement_sessions",[]).append({"started_utc":now()});write_json(suite_path,suite)
        for number,case in enumerate(suite["cases"],1):
            directory=output/case["name"]
            if case["status"]=="completed":
                clean_completed_inputs(case,directory,args.keep_data);write_json(directory/"case.json",case);write_json(suite_path,suite);continue
            verify_sources(binary,baseline,suite)
            preserve_incomplete_measurement(case,directory)
            case["status"]="measuring";write_json(directory/"case.json",case);write_json(suite_path,suite)
            print(f'MEASURE [{number}/{len(suite["cases"])}] {case["name"]}',flush=True)
            measure_case(case,directory);verify_sources(binary,baseline,suite)
            # Commit the complete result before deleting regenerable input data.
            write_json(directory/"case.json",case);write_json(suite_path,suite)
            clean_completed_inputs(case,directory,args.keep_data)
            write_json(directory/"case.json",case);write_json(suite_path,suite)
        suite["status"]="completed";suite["completed_utc"]=now();suite["measurement_sessions"][-1]["completed_utc"]=suite["completed_utc"]
    except Exception as error:
        suite["status"]="failed";suite["failure"]={"at_utc":now(),"message":str(error)};write_json(suite_path,suite);raise
    write_json(suite_path,suite)
    print(json.dumps({"status":suite["status"],"cases":len(suite["cases"]),"matrix":str(output)},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,RuntimeError,ValueError) as error:
        print(str(error),file=sys.stderr);raise SystemExit(1)
