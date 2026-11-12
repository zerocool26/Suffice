#!/usr/bin/env python3
"""Audit retained carry-state measurements without rerunning native kernels."""
from __future__ import annotations

import argparse
from collections import Counter
import datetime
import json
from pathlib import Path

from benchmark_v0_2 import digest, write_json
from benchmark_v0_6 import CANDIDATES, PREFIX, PROOF_BUDGETS, RAW, header_metadata, verify_native_artifacts
from codegen_support import ROOT
from summarize_v0_4 import extent, validate_orders, validate_samples
from summarize_v0_5 import audit_diagnostic


def require(condition,message):
    if not condition:raise ValueError(message)


def audit_available_sources(suite):
    """Check retained identities when their source artifacts are accessible."""
    inventories={"current_sources":{str(ROOT/p):h for p,h in suite["provenance"]["inputs"].items()},
                 "frozen_sources":{str(ROOT/"baseline/v0_5"/p):h for p,h in suite["frozen_sources"].items()},
                 "generating_binaries":{v["path"]:v["sha256"] for v in suite["binaries"].values()}}
    compiler=suite["provenance"]["native_compiler"]
    inventories["compiler_binary"]={compiler["executable"]:compiler["compiler_sha256"]}
    records={}
    for label,inventory in inventories.items():
        checked=[];unavailable=[]
        for name,expected in inventory.items():
            path=Path(name)
            if not path.is_file():unavailable.append(name);continue
            require(digest(path)==expected,"available provenance artifact changed: "+name)
            checked.append(name)
        records[label]={"matched":len(checked),"unavailable":unavailable}
    return records


def summarize_case(case,directory):
    result=case["native_result"];m=result["measurements"]
    require(case["status"]==result["status"]=="completed" and m["exact"],"requires complete exact native case")
    require(json.loads((directory/"native/result.json").read_text())==result,"case/native result copies disagree")
    require(json.loads((directory/"case.json").read_text())==case,"suite/case copies disagree")
    verify_native_artifacts(result,directory/"native")
    candidates={c["backend"]:c for c in m["candidates"]}
    require(len(candidates)==len(CANDIDATES) and set(candidates)==set(CANDIDATES),"candidate inventory changed")
    for c in candidates.values():validate_samples(c,m["repeat"])
    order=validate_orders(candidates,m["timing_order"],m["timing_cycle_rows"],True)
    require(len(m["timing_order"])==m["repeat"],"sample/order round inventory mismatch")
    require(m["fresh_mask_checks"]==len(candidates)*m["repeat"],"fresh full-mask inventory mismatch")
    require(m["complete_timing_cycles"]==order["complete_cycles"],"native cycle inventory mismatch")
    starts=(0,1,7,31,63,64,255,case["rows"]-65 if case["rows"]>65 else case["rows"],case["rows"])
    expected_ranges=len(candidates)*4*sum(x<=case["rows"] for x in starts)
    require(m["range_mask_checks"]==expected_ranges,"native range matrix incomplete")
    for key in ("raw_sha256","packed_sha256","program_sha256"):
        require(result["inputs"][key]==case[key],"case/native input identity mismatch: "+key)
    require(digest(directory/"program.suf")==case["program_sha256"],"retained program changed")
    if case["data_retained"]:
        require(digest(directory/"input.i32")==case["raw_sha256"] and digest(directory/"input.sfp")==case["packed_sha256"],"retained input changed")
    diagnostics={name:candidates[name]["prefix_diagnostic"] for name in PREFIX}
    for name,diagnostic in diagnostics.items():audit_diagnostic(name,diagnostic,case["rows"])
    for key in ("ambiguous_rows","certified_rows","dirty_words","vector_batches","fallback_batches",
                "scalar_batches","scalar_fallback_batches","physical_refined_rows"):
        require(len({v[key] for v in diagnostics.values()})==1,"matched generated bound/branch geometry differs: "+key)
    metadata={v["key"]:v["header_metadata"] for v in result["variants"]}
    for variant in result["variants"]:
        current=header_metadata(directory/"native"/variant["header"])
        require(current==variant["header_metadata"],"header metadata changed")
        emitted=variant["emission"]
        require(json.loads((directory/"native"/f'emission_{variant["key"]}.json').read_text())==emitted,"retained emission metadata differs")
        require(emitted["encoded_strategy"]==variant["strategy"],"strategy metadata mismatch")
        require(current["encoded_strategy"]==variant["strategy"],"generated strategy constant mismatch")
        require(current["encoded_schedule"]=="pressure","encoded schedule changed")
        if "prefix" in variant:require(current["encoded_prefix_bits"]==12,"fixed prefix depth changed")
        for key,value in current.items():
            if key in emitted:require(value==emitted[key],"header/CLI metadata mismatch: "+key)
        if not variant["baseline"]:
            require(current["encoded_carry_placement"]==variant["placement"],"carry placement changed")
        if variant["strategy"]=="guarded-residual":
            require(all(emitted.get(k)==v for k,v in PROOF_BUDGETS.items()),"fixed default proof budgets changed")
            initial=current["encoded_residual_initial_carries"];remaining=current["encoded_residual_carries"]
            accepted=current["encoded_proof_accepted_rewrites"]
            require(0<=remaining<=initial,"care optimization increased carry count")
            require(accepted<=current["encoded_proof_attempts"],"accepted rewrite count exceeds attempts")
            require(accepted<=initial-remaining,"accepted rewrite did not strictly reduce carries")
            require(current["encoded_residual_scope"] in {"all-lanes","unresolved-lanes"},"unknown residual scope")
            require(current["encoded_residual_exact_all_lanes"]==(current["encoded_residual_scope"]=="all-lanes"),"scope flag mismatch")
    # Placement changes execution scheduling, not the semantic residual plan.
    same_fields=("encoded_prefix_nodes","encoded_prefix_planes","encoded_residual_nodes","encoded_residual_planes",
                 "encoded_residual_carries","encoded_residual_initial_carries","encoded_residual_scope",
                 "encoded_proof_attempts","encoded_proof_accepted_rewrites","encoded_proof_bdd_nodes",
                 "encoded_proof_steps","encoded_proof_exhausted","encoded_proof_graph_budget_rejections")
    for left,right in (("residual_eager","residual_guarded"),("care_eager","care_guarded")):
        for key in same_fields:require(metadata[left][key]==metadata[right][key],"placement changed residual compilation: "+key)
    median=lambda n:candidates[n]["median_ms"]
    best_raw=min(RAW,key=median)
    pairs={
        "frozen_over_current_all_eager":("v05-residual-eager","v06-residual-eager"),
        "placement_only":("v06-residual-eager","v06-residual-guarded"),
        "care_only":("v06-residual-eager","v06-care-eager"),
        "care_with_guarded_placement":("v06-residual-guarded","v06-care-guarded"),
        "placement_with_care":("v06-care-eager","v06-care-guarded"),
        "all_eager_over_care_guarded":("v06-residual-eager","v06-care-guarded"),
        "frozen_over_combined":("v05-residual-eager","v06-care-guarded"),
        "frontier_over_combined":("v06-frontier","v06-care-guarded"),
        "eager_over_combined":("v06-eager","v06-care-guarded"),
        "best_raw_over_combined":(best_raw,"v06-care-guarded"),
        "portable_wordcode_over_combined":("portable-wordcode","v06-care-guarded")}
    ratios={name:median(a)/median(b) for name,(a,b) in pairs.items()}
    structural_fields=("encoded_prefix_nodes","encoded_prefix_planes","encoded_residual_nodes","encoded_residual_planes",
                       "encoded_residual_carries","encoded_full_live_values","encoded_prefix_live_values","encoded_residual_live_values")
    frozen_structure_matches={key:metadata["frozen_residual"].get(key)==metadata["residual_eager"].get(key) for key in structural_fields}
    return {"case":case["name"],"family":case["family"],"category":case["category"],"pattern":case["pattern"],
            "rows":case["rows"],"channels":case["channels"],"accepted":m["accepted"],"mask_checksum":m["mask_checksum"],
            "best_raw_backend":best_raw,"best_raw_ms":median(best_raw),"ratios":ratios,
            "ratio_operands":{k:{"numerator":a,"denominator":b} for k,(a,b) in pairs.items()},
            "candidates":m["candidates"],"diagnostics":diagnostics,"header_metadata":metadata,
            "emitted_metadata":{v["key"]:v["emission"] for v in result["variants"]},
            "frozen_current_structure_matches":frozen_structure_matches,
            "timing_order_audit":order,"fresh_mask_checks":m["fresh_mask_checks"],"range_mask_checks":m["range_mask_checks"],
            "setup":{"packing_process_ms":case["packing"]["process_wall_ms"],"input_validation_ms":m["validation_ms"],
                     "plan_ms":m["plan_ms"],"oracle_ms":m["oracle_ms"],"aos_copy_ms":m["aos_copy_ms"],
                     "soa_transpose_ms":m["soa_transpose_ms"],"copy_bytes_each":m["aos_copy_bytes"],
                     "generated_prepare_ms":m["generated_prepare_ms"],"core_prepare_ms":m["core_prepare_ms"],
                     "emission_process_ms":{v["key"]:v["emission_process_wall_ms"] for v in result["variants"]},
                     "generation_ms":{v["key"]:v["emission"]["generation_ms"] for v in result["variants"]},
                     "wrapper_compile_ms":{v["key"]:v["compilation"]["wall_ms"] for v in result["variants"]},
                     "driver_compile_ms":result["driver_compilation"]["wall_ms"],"link_ms":result["link"]["wall_ms"],
                     "support_this_invocation_ms":result["shared_support_library"]["this_invocation_build_ms"]},
            "measurement_started_utc":case["measurement_started_utc"],"measurement_completed_utc":case["measurement_completed_utc"],
            "discarded_measurements":case.get("discarded_measurements",[])}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--matrix",type=Path,required=True);parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();matrix=args.matrix.resolve();suite_path=matrix/"suite.json";suite=json.loads(suite_path.read_text())
    require(suite["schema_version"]==6 and suite["status"]=="completed","requires completed schema6 suite")
    require(suite["configuration"]["proof_budgets"]==PROOF_BUDGETS,"suite proof budgets changed")
    freshness=audit_available_sources(suite)
    require([c["name"] for c in suite["cases"]]==[d["name"] for d in suite["configuration"]["definitions"]],"case definition/order inventory changed")
    rows=[summarize_case(case,matrix/case["name"]) for case in suite["cases"]]
    for row in rows:
        require(datetime.datetime.fromisoformat(suite["all_compilation_completed_utc"])<=datetime.datetime.fromisoformat(row["measurement_started_utc"]),"recorded compilation completion is after measurement start")
    require(datetime.datetime.fromisoformat(suite["objective_and_workload_frozen_utc"])<=datetime.datetime.fromisoformat(suite["all_compilation_completed_utc"]),"objective/workload freeze follows compilation completion")
    spatial={r["pattern"]:r for r in rows if r["category"]=="spatial"};pair=None
    if set(spatial)=={"spatial_scattered","spatial_clustered"}:
        audit=json.loads((matrix/"spatial_input_audit.json").read_text())
        require(audit["actual_raw_rows_checked"] and audit["row_multisets_equal"],"missing actual spatial input check")
        unknown=spatial["spatial_scattered"]["rows"]//256
        require(len({r["accepted"] for r in spatial.values()})==1,"spatial accepted counts differ")
        for pattern,row in spatial.items():
            case=next(c for c in suite["cases"] if c["name"]==row["case"])
            require(audit["cases"][pattern]["raw_sha256"]==case["raw_sha256"],"spatial audit/raw identity mismatch")
            for d in row["diagnostics"].values():
                require(d["ambiguous_rows"]==unknown,"spatial uncertainty count differs")
                dirty=row["rows"]//256 if pattern=="spatial_scattered" else (unknown+63)//64
                require(d["dirty_words"]==dirty,"spatial dirty-word count differs")
                if d["vector_batches"]:
                    batches=row["rows"]//256 if pattern=="spatial_scattered" else (unknown+255)//256
                    require(d["fallback_batches"]==batches,"spatial repair batch count differs")
        pair={"actual_raw_row_multisets_verified_equal":True,"uncertain_rows":unknown,
              "uncertain_fraction":unknown/spatial["spatial_scattered"]["rows"],"input_audit_sha256":digest(matrix/"spatial_input_audit.json")}
    counts={"cases":len(rows),"candidate_count":len(CANDIDATES),"categories":dict(Counter(r["category"] for r in rows)),
            "fresh_full_masks":sum(r["fresh_mask_checks"] for r in rows),"range_masks":sum(r["range_mask_checks"] for r in rows),
            "complete_native_cycles":sum(r["timing_order_audit"]["complete_cycles"] for r in rows)}
    ranges={}
    for category in sorted({r["category"] for r in rows}):
        group=[r for r in rows if r["category"]==category]
        ranges[category]={key:extent(r["ratios"][key] for r in group) for key in group[0]["ratios"]}
    summary={"schema_version":6,"suite_sha256":digest(suite_path),"configuration":suite["configuration"],"binaries":suite["binaries"],
             "counts":counts,"ranges":ranges,"spatial_pair":pair,"cases":rows,"source_freshness_audit":freshness,
             "resource_measurement_scope":{"compiler_wall_time_observed":True,"isolated_compiler_peak_memory_measured":False,
                                           "isolated_proof_engine_peak_memory_measured":False},
             "chronology":{"objective_and_workload_frozen_utc":suite["objective_and_workload_frozen_utc"],
                           "all_compilation_completed_utc":suite["all_compilation_completed_utc"],
                           "last_preflight_completed_utc":suite["last_preflight_completed_utc"],"measurement_sessions":suite["measurement_sessions"]}}
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True);write_json(output/"SUMMARY.json",summary)
    lines=["# Suffice 0.6 carry-state experiment","",
           "All time ratios divide medians from the same native process. Above one favors the denominator. Depth12 and pressure scheduling are fixed. "
           "Eager/guarded placement and all-lane/care residual compilation form two independent factors. No measured winner becomes an automatic policy.","",
           "## Native medians","",
           "| Case | Frozen05 residual | Current all/eager | Current all/guarded | Care/eager | Care/guarded | Frontier | Eager | Best raw |",
           "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    display=("v05-residual-eager","v06-residual-eager","v06-residual-guarded","v06-care-eager","v06-care-guarded","v06-frontier","v06-eager")
    for row in rows:
        c={v["backend"]:v for v in row["candidates"]}
        lines.append('| '+row["case"]+' | '+' | '.join(f'{c[n]["median_ms"]:.6f}' for n in display)+f' | {row["best_raw_ms"]:.6f} |')
    lines += ["","Values are milliseconds. Best raw is a reporting envelope of four named AoS/SoA scalar/SIMD controls, not a deployed selector. "
              "The complete samples and portable-wordcode control are in SUMMARY.json.","","## Isolating the changes","",
              "| Case | All eager / all guarded | All eager / care eager | All eager / care guarded | Frozen05 / care guarded | Frontier / care guarded | Best raw / care guarded |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    keys=("placement_only","care_only","all_eager_over_care_guarded","frozen_over_combined","frontier_over_combined","best_raw_over_combined")
    for row in rows:lines.append('| '+row["case"]+' | '+' | '.join(f'{row["ratios"][k]:.3f}x' for k in keys)+' |')
    lines += ["","The combined columns are speedups: numerator is the stated control and denominator is care/guarded. The JSON names both operands. "
              "Ordinary, stress, spatial, and structural cases remain separate in aggregate ranges.","","## Retained state and proof work","",
              "| Case | Carries before | Carries after | Prefix live eager→care/guarded | Suffix nodes before→after | Proof attempts | Accepted | BDD nodes | Proof steps | Budget exhausted |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"]
    for row in rows:
        a=row["header_metadata"]["residual_eager"];b=row["header_metadata"]["care_guarded"]
        lines.append(f'| {row["case"]} | {b["encoded_residual_initial_carries"]} | {b["encoded_residual_carries"]} | '
                     f'{a["encoded_prefix_live_values"]}→{b["encoded_prefix_live_values"]} | {a["encoded_residual_nodes"]}→{b["encoded_residual_nodes"]} | '
                     f'{b["encoded_proof_attempts"]} | {b["encoded_proof_accepted_rewrites"]} | {b["encoded_proof_bdd_nodes"]} | '
                     f'{b["encoded_proof_steps"]} | {b["encoded_proof_exhausted"]} |')
    lines += ["","Carry counts and source live-value models are not measured stack/RSS/register requirements. Proof counters include bounded attempted work; "
              "the emitted reason, graph-budget rejections, exactness scope, and generation duration remain in raw metadata. "
              "Care residual helpers are exact where the root uncertainty mask is true; the prepared outer execution remains exact on all output lanes.","",
              "## Physical repair and generation cost","",
              "| Case | Uncertain rows | Dirty64 words | Dirty256 batches | Physical repair rows | All/eager generation ms | Care/guarded generation ms |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        d=row["diagnostics"]["v06-care-guarded"];g=row["setup"]["generation_ms"]
        lines.append(f'| {row["case"]} | {d["ambiguous_rows"]:,} | {d["dirty_words"]:,} | {d["fallback_batches"]:,} | '
                     f'{d["physical_refined_rows"]:,} | {g["residual_eager"]:.3f} | {g["care_guarded"]:.3f} |')
    lines += ["","Generation duration is the CLI's complete generation phase, including its validation/witness work; it is not an isolated theorem-prover stopwatch. "
              "Emission process wall time additionally includes input validation and process overhead. All packing, copy, transpose, compilation, linking, "
              "and prepared-instance costs are retained separately. Isolated compiler and proof-engine peak memory are not measured. "
              "Research setup contains overlapping work for multiple controls and is not a deployment break-even sum.","",
              "## Audit and scope","",
              f'{counts["fresh_full_masks"]:,} fresh complete masks and {counts["range_masks"]:,} arbitrary-range masks were checked against original scalar arithmetic. '
              f'{counts["complete_native_cycles"]} complete native Williams cycles passed position/predecessor balance. '
              "The summary reconstructs all medians, verifies header/object/binary identities, checks emitted/header metadata agreement, compares root-bound counts/branch geometry, "
              "and verifies input and case-record identities. Original compilation completion is retained across resume, and incomplete-case outputs are archived before rerun.","",
              "Accessible current/frozen sources, generating binaries, and the compiler binary are rehashed in the source freshness audit. "
              "Unavailable paths after relocation are reported explicitly and are not counted as reverified.","",
              "All candidates coexist in the harness. Timings are warm synthetic Linux/compiler observations with no confidence-interval or Windows/end-to-end-AI superiority claim. "
              "Allocation/sanitizer/compiler-proof validation belongs to the separately retained correctness suites, not these timing counters.",""]
    (output/"TABLES.md").write_text("\n".join(lines),encoding="utf-8")
    print(json.dumps(counts,indent=2))


if __name__=="__main__":main()
