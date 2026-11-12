#!/usr/bin/env python3
"""Read-only audit of the grouped-binding experiment; no kernel execution."""
from __future__ import annotations
import argparse
from collections import Counter
import datetime
import json
import math
from pathlib import Path
import statistics

from benchmark_v0_2 import digest,write_json
from benchmark_v0_8 import (BINDING_NAMES,BINDING_PASSES,CANDIDATES,PREFIX,PROOF_BUDGETS,RAW,
                            encoded_bodies,header_metadata,verify_native_artifacts)
from codegen_support import ROOT
from summarize_v0_4 import validate_orders,validate_samples
from summarize_v0_5 import audit_diagnostic


def require(value,message):
    if not value:raise ValueError(message)


def close(actual,expected,message):
    require(math.isfinite(actual) and math.isclose(actual,expected,rel_tol=1e-12,abs_tol=1e-9),message)


def numeric_range(values):
    values=list(values)
    return {"min":min(values),"max":max(values),"median":statistics.median(values),
            "geometric_mean":math.exp(statistics.mean(math.log(v) for v in values))}


def paired(a,b,cycle):
    """Descriptive pairing by round; not a confidence interval or independence claim."""
    require(len(a)==len(b) and len(a)%cycle==0,"incomplete paired observations")
    ratios=[x/y for x,y in zip(a,b)]
    return {"ratio_of_medians":statistics.median(a)/statistics.median(b),
            "median_of_same_round_ratios":statistics.median(ratios),
            "same_round_ratios":ratios,
            "complete_cycle_median_ratios":[statistics.median(a[i:i+cycle])/statistics.median(b[i:i+cycle])
                                           for i in range(0,len(a),cycle)],
            "scope":"Same-round descriptive pairing under balanced order; calls are sequential, not simultaneous; no significance claim."}


def available_sources(suite):
    inventories={
        "current_sources":{str(ROOT/k):v for k,v in suite["provenance"]["inputs"].items()},
        "frozen_sources":{str(ROOT/"baseline/v0_7"/k):v for k,v in suite["frozen_sources"].items()},
        "generators":{v["path"]:v["sha256"] for v in suite["binaries"].values()},
        "frozen_archive":{str(ROOT/suite["frozen_release"]["path"]):suite["frozen_release"]["sha256"]},
        "compiler":{suite["provenance"]["native_compiler"]["executable"]:suite["provenance"]["native_compiler"]["compiler_sha256"]}}
    results={}
    for label,inventory in inventories.items():
        matched=0;unavailable=[]
        for name,expected in inventory.items():
            path=Path(name)
            if path.is_file():
                require(digest(path)==expected,"available provenance file changed: "+name);matched+=1
            else:unavailable.append(name)
        results[label]={"matched":matched,"unavailable":unavailable}
    return results


def binding_probe(d,rows,block_rows,slots,calls):
    blocks=(rows+block_rows-1)//block_rows
    complete,tail=divmod(rows,block_rows)
    logical_bytes=(complete*((block_rows+7)//8)+(tail+7)//8)*slots
    require(d["allocations"]==0,"binding/scanning probe allocated")
    require(d["blocks"]==blocks and d["plane_slots"]==slots,"binding inventory mismatch")
    require(d["expected_binding_calls_per_block"]==calls,"binding call metadata differs")
    require(d["observed_plane_view_calls"]==blocks*calls,"actual plane_view count differs")
    require(d["requested_planes"]==blocks*slots,"requested plane count differs")
    require(d["returned_view_bytes"]==logical_bytes,"logical returned byte extents differ")
    return {"blocks":blocks,"plane_slots":slots,"view_calls":blocks*calls,
            "requested_planes":blocks*slots,"logical_view_bytes":logical_bytes}


def summarize_case(case,directory,suite):
    require(case["status"]=="completed","incomplete case")
    require(json.loads((directory/"case.json").read_text())==case,"case record differs from suite")
    result=case["native_result"];m=result["measurements"]
    require(json.loads((directory/"native/result.json").read_text())==result,"native result differs from suite")
    require(json.loads((directory/"native/measurements.json").read_text())==m,"measurements differ from result")
    verify_native_artifacts(result,directory/"native")
    require(result["status"]=="completed" and m["exact"],"native execution incomplete")
    require(m["measurement_purpose"]==suite["configuration"]["measurement_purpose"],"purpose mismatch")
    require(m["rows"]==case["rows"] and m["channels"]==case["channels"] and m["block_rows"]==case["block_rows"],"shape mismatch")
    require(m["clock_is_steady"] and m["clock_period_num"]>0 and m["clock_period_den"]>0,"clock metadata invalid")
    require(result["binaries"]==suite["cases"][0]["native_result"]["binaries"],"generator identity changed across cases")
    require(result["inputs"]["raw_sha256"]==case["raw_sha256"] and result["inputs"]["packed_sha256"]==case["packed_sha256"],"input identity mismatch")
    require(digest(directory/"program.suf")==case["program_sha256"]==result["inputs"]["program_sha256"],"program identity mismatch")
    for filename,key in (("input.i32","raw_sha256"),("input.sfp","packed_sha256")):
        p=directory/filename
        if p.is_file():require(digest(p)==case[key],"retained input changed")
    for name,expected in result["harness_sources"].items():
        p=ROOT/name
        if p.is_file():require(digest(p)==expected,"available harness input changed")
    candidates={c["backend"]:c for c in m["candidates"]}
    require(set(candidates)==set(CANDIDATES) and len(m["candidates"])==len(CANDIDATES),"full-scan inventory changed")
    repeat=m["repeat"];cycle=m["timing_cycle_rows"]
    require(repeat==((m["minimum_repeat"]+len(CANDIDATES)-1)//len(CANDIDATES))*len(CANDIDATES),"round count differs")
    order=validate_orders(CANDIDATES,m["timing_order"],cycle,True)
    require(m["fresh_mask_checks"]==repeat*len(CANDIDATES),"fresh mask count differs")
    starts=[0,1,7,31,63,64,255,case["rows"]-65 if case["rows"]>65 else case["rows"],case["rows"]]
    require(m["range_mask_checks"]==sum(x<=case["rows"] for x in starts)*4*len(CANDIDATES),"range check inventory differs")
    require(m["allocation_checks"]==18,"allocation probe count differs")
    require(m["complete_timing_cycles"]==order["complete_cycles"],"complete cycle count differs")
    for c in candidates.values():
        validate_samples(c,repeat)
        ns=c["samples_ns"]
        require(len(ns)==repeat and all(isinstance(n,int) and n>0 for n in ns),"integer ns samples missing")
        for n,ms in zip(ns,c["samples_ms"]):close(ms,n/1e6,"ns/ms conversion differs")
        if c["backend"] in PREFIX:
            audit_diagnostic(c["backend"],c["prefix_diagnostic"],case["rows"])
            require(c["binding_probe"]["exact_mask"],"missing counted exact mask")
        else:require(c["binding_probe"] is None,"raw control has unexpected binding probe")
    variants={v["key"]:v for v in result["variants"]}
    for key,v in variants.items():
        header=directory/"native"/v["header"]
        require(header_metadata(header)==v["header_metadata"],"header metadata differs")
        require(json.loads((directory/"native"/f"emission_{key}.json").read_text())==v["emission"],"emission metadata differs")
        if not v["encoded"]:continue
        require(encoded_bodies(header)==v["encoded_bodies"],"encoded body/slot hashes differ")
        md=v["header_metadata"];calls=md["encoded_plane_count"] if v["baseline"] else md["encoded_binding_calls_per_block"]
        binding_probe(candidates[v["label"]]["binding_probe"],case["rows"],case["block_rows"],md["encoded_plane_count"],calls)
        if not v["baseline"]:
            require(md["encoded_plane_binding"]==v["binding"],"binding mode metadata differs")
            require(md["encoded_grouped_binding_emitted"]==(v["binding"]=="grouped"),"grouped marker differs")
        if v["strategy"]=="guarded-residual":
            require(all(v["emission"][k]==value for k,value in PROOF_BUDGETS.items()),"proof budget differs")
    for semantic in ("residual","care"):
        family=[variants[semantic+"_"+mode] for mode in ("frozen","per_plane","grouped")]
        require(all(v["encoded_bodies"]==family[0]["encoded_bodies"] for v in family),"arithmetic changed across binding treatments")
        require(result["encoded_body_matches"][semantic]["all_three_exactly_equal"],"missing body match assertion")
        require(result["encoded_body_matches"][semantic]["functions"]==family[0]["encoded_bodies"]["functions"],"body audit mismatch")
        d=[candidates[v["label"]]["prefix_diagnostic"] for v in family]
        require(d[0]==d[1]==d[2],"binding changed prefix diagnostic geometry")
    ratios={};pair_records={}
    for semantic in ("residual","care"):
        for label,a,b in (
            ("current_per_plane_over_grouped",f"v08-{semantic}-per-plane",f"v08-{semantic}-grouped"),
            ("frozen_over_current_per_plane",f"v07-{semantic}-per-plane",f"v08-{semantic}-per-plane"),
            ("frozen_over_grouped",f"v07-{semantic}-per-plane",f"v08-{semantic}-grouped")):
            key=semantic+"_"+label
            pair_records[key]={"numerator":a,"denominator":b,**paired(candidates[a]["samples_ns"],candidates[b]["samples_ns"],cycle)}
            ratios[key]=pair_records[key]["ratio_of_medians"]
    best_raw=min(RAW,key=lambda n:candidates[n]["median_ms"])
    for semantic in ("residual","care"):
        ratios["best_raw_over_"+semantic+"_grouped"]=candidates[best_raw]["median_ms"]/candidates[f"v08-{semantic}-grouped"]["median_ms"]
    binding=m["binding_experiment"]
    require(binding["passes_per_sample"]==BINDING_PASSES and binding["repeat"]==repeat,"binding repetition count differs")
    binding_order=validate_orders(BINDING_NAMES,binding["timing_order"],2,True)
    require(binding_order["complete_cycles"]==binding["complete_timing_cycles"],"binding cycle count differs")
    bc={c["name"]:c for c in binding["candidates"]}
    require(set(bc)==set(BINDING_NAMES) and len(binding["candidates"])==2,"binding inventory differs")
    for name,c in bc.items():
        require(len(c["samples_ns"])==repeat and all(isinstance(n,int) and n>0 for n in c["samples_ns"]),"invalid binding ns samples")
        key="residual_per_plane" if name=="binding-per-plane" else "residual_grouped"
        md=variants[key]["header_metadata"];d=c["diagnostic"]
        binding_probe(d,case["rows"],case["block_rows"],md["encoded_plane_count"],md["encoded_binding_calls_per_block"])
        require(d["original_slot_identities_checked"] and d["slots_checked"]==d["blocks"]*d["plane_slots"],"slot identity audit incomplete")
        require(d["rows"]==case["rows"],"binding row extent mismatch")
        require(len(d["plane_contracts"])==md["encoded_plane_count"] and len({tuple(v) for v in d["plane_contracts"]})==md["encoded_plane_count"],"duplicate or missing plane identities")
        require(all(0<=ch<case["channels"] and 0<=bit<32 for ch,bit in d["plane_contracts"]),"invalid plane identities")
        c["median_ns_per_pass"]=statistics.median(c["samples_ns"])/BINDING_PASSES
        c["median_ms_per_pass"]=c["median_ns_per_pass"]/1e6
    require(bc[BINDING_NAMES[0]]["diagnostic"]["plane_contracts"]==bc[BINDING_NAMES[1]]["diagnostic"]["plane_contracts"],"binder consumer slots changed")
    bind_comparison=paired(bc[BINDING_NAMES[0]]["samples_ns"],bc[BINDING_NAMES[1]]["samples_ns"],2)
    setup={key:m[key] for key in ("validation_ms","plan_ms","oracle_ms","aos_copy_ms","soa_transpose_ms",
                                  "aos_copy_bytes","soa_copy_bytes","generated_prepare_ms","range_validation_ms","prefix_diagnostic_ms")}
    setup.update({"packing_process_ms":case["packing"]["process_wall_ms"],
                  "wrapper_compile_ms":{v["key"]:v["compilation"]["wall_ms"] for v in result["variants"]},
                  "generation_ms":{v["key"]:v["emission"]["generation_ms"] for v in result["variants"]},
                  "emission_process_ms":{v["key"]:v["emission_process_wall_ms"] for v in result["variants"]},
                  "driver_compile_ms":result["driver_compilation"]["wall_ms"],"sink_compile_ms":result["sink_compilation"]["wall_ms"],
                  "link_ms":result["link"]["wall_ms"]})
    return {"case":case["name"],"family":case["family"],"category":case["category"],"pattern":case["pattern"],
            "rows":case["rows"],"channels":case["channels"],"block_rows":case["block_rows"],
            "measurement_purpose":m["measurement_purpose"],"accepted":m["accepted"],"mask_checksum":m["mask_checksum"],
            "ratios":ratios,"paired_comparisons":pair_records,"best_raw_backend":best_raw,"best_raw_ms":candidates[best_raw]["median_ms"],
            "candidates":m["candidates"],"binding":{"order_audit":binding_order,"comparison":bind_comparison,
                "candidates":list(bc.values()),"passes_per_sample":BINDING_PASSES,"scope":binding["scope"]},
            "encoded_body_matches":result["encoded_body_matches"],"timing_order_audit":order,
            "fresh_mask_checks":m["fresh_mask_checks"],"range_mask_checks":m["range_mask_checks"],
            "allocation_checks":m["allocation_checks"],"setup":setup,
            "measurement_started_utc":case["measurement_started_utc"],"measurement_completed_utc":case["measurement_completed_utc"],
            "discarded_measurements":case.get("discarded_measurements",[])}


def tables(summary):
    rows=summary["cases"]
    lines=["# Suffice0.8 grouped plane binding — audited observations","",
           "Ratios are numerator median / denominator median. Above1 favors the denominator. "
           "These are warm same-input observations from one linked process per case; no confidence intervals or default-promotion claim.","",
           "## Full scans with unchanged encoded arithmetic","",
           "| Case | Block rows | All: per-plane/grouped | Care: per-plane/grouped | Frozen/current all control | Frozen/current care control | Best raw/care grouped |",
           "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    keys=("residual_current_per_plane_over_grouped","care_current_per_plane_over_grouped",
          "residual_frozen_over_current_per_plane","care_frozen_over_current_per_plane","best_raw_over_care_grouped")
    for r in rows:lines.append("| "+r["case"]+" | "+str(r["block_rows"])+" | "+" | ".join(f'{r["ratios"][k]:.6f}×' for k in keys)+" |")
    lines+=["","## Absolute full-scan medians (milliseconds)","",
            "| Case | Frozen all | Current all | Grouped all | Frozen care | Current care | Grouped care | Best raw |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for r in rows:
        c={v["backend"]:v for v in r["candidates"]}
        order=("v07-residual-per-plane","v08-residual-per-plane","v08-residual-grouped","v07-care-per-plane","v08-care-per-plane","v08-care-grouped")
        lines.append("| "+r["case"]+" | "+" | ".join(f'{c[k]["median_ms"]:.6f}' for k in order)+f' | {r["best_raw_ms"]:.6f} |')
    lines+=["","## Binding plus observable slot-consumer cost","",
            "Each sample binds32 full passes. The medians below divide that sampled interval by32. "
            "Every block includes an out-of-line sink reading all pointer/size pairs; no input payload or Boolean result is evaluated. "
            "These values cannot be subtracted from independently measured full scans to recover a kernel-only latency.","",
            "| Case | Per-plane ms/pass | Grouped ms/pass | Per-plane/grouped | View calls/pass: per-plane → grouped | Same logical view bytes/pass |",
            "| --- | ---: | ---: | ---: | --- | ---: |"]
    for r in rows:
        a,b=r["binding"]["candidates"]
        lines.append(f'| {r["case"]} | {a["median_ms_per_pass"]:.6f} | {b["median_ms_per_pass"]:.6f} | '
                     f'{r["binding"]["comparison"]["ratio_of_medians"]:.6f}× | '
                     f'{a["diagnostic"]["observed_plane_view_calls"]:,} → {b["diagnostic"]["observed_plane_view_calls"]:,} | '
                     f'{a["diagnostic"]["returned_view_bytes"]:,} |')
    lines+=["","## Descriptive paired observations","",
            "SUMMARY.json retains raw integer-nanosecond samples, same-round ratios, their median, and each complete-cycle median ratio. "
            "Pairing sequential calls by round does not make them simultaneous or independent and is not a significance test.","",
            "## Scope and promotion gate","",
            "The fixed matrix includes nine retained workloads plus two separately labeled block-size sensitivity cases using identical raw bytes and query. "
            "The stronger0.7 default-promotion protocol (three fresh processes and two link layouts) is not performed by this initial efficacy matrix. "
            "PerPlane remains the default; all observed gains/regressions and unchanged-control drift are retained.",""]
    return "\n".join(lines)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--matrix",type=Path,required=True);p.add_argument("--output",type=Path,required=True)
    args=p.parse_args();matrix=args.matrix.resolve();output=args.output.resolve()
    require(not output.exists(),"use a fresh summary output")
    suite=json.loads((matrix/"suite.json").read_text());require(suite["status"]=="completed","matrix incomplete")
    require(suite["configuration"]["candidate_inventory"]==list(CANDIDATES),"suite candidate inventory changed")
    require(suite["configuration"]["binding_inventory"]==list(BINDING_NAMES),"suite binding inventory changed")
    require(suite["configuration"]["binding_passes_per_sample"]==BINDING_PASSES,"suite binding passes differ")
    require(suite["configuration"]["proof_budgets"]==PROOF_BUDGETS,"suite proof limits differ")
    require(suite["all_compilation_completed_utc"]<=suite["measurement_sessions"][0]["started_utc"],"timing preceded compilation")
    cases=[summarize_case(c,matrix/c["name"],suite) for c in suite["cases"]]
    require(len({c["case"] for c in cases})==len(cases),"duplicate case")
    for c in cases:require(c["measurement_started_utc"]>=suite["all_compilation_completed_utc"],"case timing preceded compile")
    input_audits={}
    for name in ("spatial_input_audit.json","block_input_audit.json"):
        path=matrix/name
        if path.exists():input_audits[name]={"sha256":digest(path),"record":json.loads(path.read_text())}
    spatial=[c for c in cases if c["category"]=="spatial"]
    if len(spatial)==2:
        require(input_audits["spatial_input_audit.json"]["record"]["row_multisets_equal"],"spatial row multiset audit missing")
        ds=[next(x for x in c["candidates"] if x["backend"]=="v08-care-grouped")["prefix_diagnostic"] for c in spatial]
        require(ds[0]["ambiguous_rows"]==ds[1]["ambiguous_rows"]==spatial[0]["rows"]//256,"spatial uncertainty changed")
    block_cases=[c for c in cases if c["category"]=="block_sensitivity"]
    if block_cases:
        require(input_audits["block_input_audit.json"]["record"]["same_raw_rows_and_program"],"block sensitivity identity audit missing")
        require(input_audits["block_input_audit.json"]["record"]["same_encoded_arithmetic_bodies"],"block sensitivity arithmetic changed")
    groups={"all":cases,"retained_nine":[c for c in cases if c["category"]!="block_sensitivity"]}
    groups.update({k:[c for c in cases if c["category"]==k] for k in sorted({c["category"] for c in cases})})
    ranges={label:{key:numeric_range(c["ratios"][key] for c in selected) for key in selected[0]["ratios"]}
            for label,selected in groups.items() if selected}
    summary={"schema_version":8,"created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
             "suite_sha256":digest(matrix/"suite.json"),"configuration":suite["configuration"],
             "environment":suite["environment"],"binaries":suite["binaries"],"frozen_release":suite["frozen_release"],
             "source_freshness_audit":available_sources(suite),"input_audits":input_audits,
             "counts":{"cases":len(cases),"candidates":len(CANDIDATES),"categories":dict(Counter(c["category"] for c in cases)),
                       "fresh_full_masks":sum(c["fresh_mask_checks"] for c in cases),
                       "range_masks":sum(c["range_mask_checks"] for c in cases),
                       "allocation_checks":sum(c["allocation_checks"] for c in cases),
                       "complete_native_cycles":sum(c["timing_order_audit"]["complete_cycles"] for c in cases),
                       "binding_samples":sum(c["binding"]["order_audit"]["rounds"]*2 for c in cases),
                       "binding_complete_cycles":sum(c["binding"]["order_audit"]["complete_cycles"] for c in cases)},
             "ranges":ranges,"cases":cases,
             "chronology":{key:suite[key] for key in ("objective_and_workload_frozen_utc","all_compilation_completed_utc","last_preflight_completed_utc","measurement_sessions")},
             "scope":{"one_layout_and_process_per_case":True,"default_promotion_protocol_completed":False,
                      "isolated_compiler_peak_memory_measured":False,"binding_includes_observable_sink":True,
                      "binding_values_subtractable_from_scan":False}}
    output.mkdir(parents=True);write_json(output/"SUMMARY.json",summary)
    (output/"TABLES.md").write_text(tables(summary))
    print(json.dumps({"status":"PASS","counts":summary["counts"],"output":str(output)},indent=2))


if __name__=="__main__":
    try:main()
    except (OSError,ValueError,RuntimeError,KeyError) as error:
        raise SystemExit(str(error))
