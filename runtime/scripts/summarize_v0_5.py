#!/usr/bin/env python3
"""Audit v0.5 artifacts and report implementation ratios separately from selection regret."""
from __future__ import annotations

import argparse
from collections import Counter,defaultdict
import json
import math
from pathlib import Path
import statistics

from benchmark_v0_2 import digest,write_json
from reproduce_v0_5 import POLICIES,SEED_SUBSET,verify_native_artifacts
from summarize_v0_4 import validate_orders,validate_samples,extent

RAW=("raw-aos-scalar","raw-aos-simd","raw-soa-scalar","raw-soa-simd","dense","raw-vector")
PORTABLE=("v04-portable-frontier12","v05-portable-sparse12","v05-portable-hybrid12","v05-portable-wordcode")
GENERATED=("v04-generated-frontier12","v05-generated-eager","v05-generated-frontier12","v05-generated-residual12")
PREFIX_GENERATED=(GENERATED[0],GENERATED[2],GENERATED[3])


def assert_close(actual,expected,label):
    if not math.isclose(actual,expected,rel_tol=1e-8,abs_tol=1e-10):raise ValueError(label+" disagrees with retained observations")


def audit_diagnostic(name,diagnostic,rows):
    d=diagnostic
    if not d["available"]:raise ValueError("missing repair diagnostic: "+name)
    if d["certified_rows"]+d["ambiguous_rows"]!=rows or d["bounds_checked_rows"]!=rows:
        raise ValueError("prefix bound row accounting failed: "+name)
    if not 0<=d["ambiguous_rows"]<=d["physical_refined_rows"]<=rows:
        raise ValueError("logical versus physical repair rows invalid: "+name)
    if not 0<=d["dirty_words"]<=d["fallback_words"]:
        raise ValueError("dirty versus physical repair words invalid: "+name)
    if d["from_execution_counters"]:
        if d["sparse_words"]>d["dirty_words"]:raise ValueError("sparse repair count exceeds dirty words")
        if name!="v05-portable-hybrid12" and not d["dirty_words"]==d["fallback_words"]==d["sparse_words"]:
            raise ValueError("word-only portable control changed repair granularity")
        if name!="v05-portable-hybrid12" and d["dense_tiles"]:raise ValueError("word-only control used dense tiles")
    else:
        if d["vector_rows"]!=256*d["vector_batches"]:raise ValueError("vector batch row mismatch")
        if d["vector_rows"]+d["scalar_or_tail_rows"]+d["constant_fastpath_rows"]!=rows:
            raise ValueError("generated physical branch row accounting failed")
        if not 0<=d["fallback_batches"]<=d["vector_batches"] or not 0<=d["scalar_fallback_batches"]<=d["scalar_batches"]:
            raise ValueError("invalid generated fallback batch counts")
        if d["fallback_words"]!=4*d["fallback_batches"]+d["scalar_fallback_batches"]:
            raise ValueError("generated fallback word width mismatch")
        scalar_repair=d["physical_refined_rows"]-256*d["fallback_batches"]
        if not 0<=scalar_repair<=d["scalar_or_tail_rows"] or scalar_repair>64*d["scalar_fallback_batches"]:
            raise ValueError("generated scalar/tail repair width mismatch")
        if d["constant_fastpath_rows"] and (d["vector_batches"] or d["scalar_batches"] or d["physical_refined_rows"]):
            raise ValueError("constant output unexpectedly attributed batch repair")


def spatial_sample(case,selection):
    if not case["pattern"].startswith("spatial_"):return None
    total=case["rows"];uncertain=total//256
    raw_unknown=raw_rows=0;weighted_unknown=weighted_rows=0.0
    windows=[]
    for window in selection["windows"]:
        first,count=window["first_row"],window["row_count"];end=first+count
        if case["pattern"]=="spatial_clustered":unknown=max(0,min(end,uncertain)-first)
        else:unknown=(end+255)//256-(first+255)//256
        weight=window.get("timing_weight",1.0)
        raw_unknown+=unknown;raw_rows+=count
        weighted_unknown+=unknown*weight;weighted_rows+=count*weight
        windows.append({"first_row":first,"row_count":count,"unknown_rows":unknown,"timing_weight":weight})
    return {"global_unknown_rows":uncertain,"global_unknown_fraction":uncertain/total,
            "sample_unknown_rows":raw_unknown,"sample_rows":raw_rows,
            "sample_unknown_fraction":raw_unknown/raw_rows if raw_rows else None,
            "sample_to_global_unknown_fraction":(raw_unknown/raw_rows)/(uncertain/total) if raw_rows else None,
            "weighted_sample_unknown_fraction":weighted_unknown/weighted_rows if weighted_rows else None,
            "boundary_probes_excluded":True,"windows":windows}


def audit_selection(run,case):
    core=run["result"];check=core["verification"];native=case["native_result"]["measurements"]
    if not check["exact_match"] or check["mask_checksum"]!=native["mask_checksum"] or check["accepted"]!=native["accepted"]:
        raise ValueError("selector/native exactness mismatch")
    fixed=core["fixed"];names=[c["backend"] for c in fixed]+["adaptive"]
    for candidate in [*fixed,core["adaptive"]]:validate_samples(candidate,core["repeat"])
    fresh=core["fresh_execution"]
    order=validate_orders(names,fresh["round_order"],fresh["cycle_rows"],require_complete=True)
    if check["heldout_mask_checks"]!=len(names)*core["repeat"]:raise ValueError("incorrect fresh selector mask count")
    if fresh["partial_cycle_rounds"] or fresh["complete_cycles"]!=order["complete_cycles"]:
        raise ValueError("incomplete/misreported selector cycles")
    selection=core["selection"]
    if selection["sample_seed"]!=run["seed"]:raise ValueError("selection seed mismatch")
    if selection["total_calibration_row_evaluations"]>selection["calibration_row_budget"]:
        raise ValueError("requested calibration rows exceeded budget")
    if selection["total_calibration_row_evaluations"]!=sum(selection[k] for k in ("oracle_row_evaluations","warmup_row_evaluations","timed_row_evaluations")):
        raise ValueError("calibration work accounting mismatch")
    policy=run["policy"]
    expected_policy="representative" if policy=="current-representative" else "legacy-endpoints"
    if not policy.startswith("frozen") and selection["sample_policy"]!=expected_policy:
        raise ValueError("requested/current policy mismatch")
    # Every timed window must be in range and distinct/nonoverlapping. Only
    # current representative claims selected-grain alignment and weighting.
    windows=selection["windows"]
    intervals=sorted((w["first_row"],w["first_row"]+w["row_count"]) for w in windows)
    if any(first<0 or first>=end or end>case["rows"] for first,end in intervals):raise ValueError("invalid timed calibration window")
    if any(intervals[i][1]>intervals[i+1][0] for i in range(len(intervals)-1)):
        raise ValueError("overlapping timed calibration windows")
    if sum(w["row_count"] for w in windows)!=selection["calibrated_rows"]:
        raise ValueError("timed calibration row sum mismatch")
    for candidate in selection["candidates"]:
        scores=candidate["samples_ms"]
        durations=candidate["window_samples_ms"]
        if len(durations)!=len(scores)*len(windows):
            raise ValueError("calibration window-duration inventory mismatch")
        for index,score in enumerate(scores):
            observed=durations[index*len(windows):(index+1)*len(windows)]
            if any(not math.isfinite(value) or value<0 for value in observed):
                raise ValueError("invalid calibration window duration")
            reconstructed=sum(value*window.get("timing_weight",1.0) for value,window in zip(observed,windows))
            assert_close(score,reconstructed,"population-weighted calibration score")
    if policy=="current-representative":
        alignment=selection["sample_alignment_rows"]
        if alignment==0:
            if intervals!=[(0,case["rows"])]:raise ValueError("zero alignment is reserved for full reference")
        elif alignment<1 or any(w["first_row"]%alignment or w["row_count"]%alignment for w in windows):
            raise ValueError("representative timed window violates declared alignment")
        if any(not math.isfinite(w["timing_weight"]) or w["timing_weight"]<=0 for w in windows):
            raise ValueError("invalid stratum timing weight")
        assert_close(sum(w["row_count"]*w["timing_weight"] for w in windows),
                     selection["calibrated_rows"],"normalized population timing weights")
        if sum(w["population_rows"] for w in windows)!=selection["sample_population_rows"]:
            raise ValueError("stratum populations do not cover declared timed population")
        if selection["sample_population_rows"]+selection["sample_unrepresented_rows"]!=case["rows"]:
            raise ValueError("timing population plus excluded tail does not equal dataset")
        probes=selection["boundary_probes"]
        if sum(p["row_count"] for p in probes)!=selection["boundary_probe_rows"]:
            raise ValueError("boundary correctness row accounting mismatch")
        if selection["boundary_oracle_row_evaluations"]>selection["oracle_row_evaluations"] or selection["boundary_candidate_row_evaluations"]>selection["warmup_row_evaluations"]:
            raise ValueError("boundary work subsets were incorrectly accounted")
    eligible={c["backend"] for c in selection["candidates"] if c["eligible"]}
    best=min((c for c in fixed if c["backend"] in eligible),key=lambda c:c["timing"]["median_ms"])
    selected=next(c for c in fixed if c["backend"]==core["adaptive"]["backend"])
    selected_regret=selected["timing"]["median_ms"]/best["timing"]["median_ms"]
    actual_regret=core["adaptive"]["timing"]["median_ms"]/best["timing"]["median_ms"]
    assert_close(core["selection_regret"]["selected_fixed_over_best_eligible_fixed"],selected_regret,"selected-fixed regret")
    assert_close(core["selection_regret"]["adaptive_over_best_eligible_fixed"],actual_regret,"actual-instance regret")
    return {"policy":policy,"seed":run["seed"],"selected":core["adaptive"]["backend"],
            "best_eligible_fixed":best["backend"],"selected_fixed_regret":selected_regret,"actual_instance_regret":actual_regret,
            "selected_fixed_ms":selected["timing"]["median_ms"],"actual_instance_ms":core["adaptive"]["timing"]["median_ms"],
            "best_eligible_fixed_ms":best["timing"]["median_ms"],"prepare_ms":core["preparation"]["adaptive_construct_wall_ms"],
            "work_rows":selection["total_calibration_row_evaluations"],"work_budget":selection["calibration_row_budget"],
            "sample_alignment_rows":selection.get("sample_alignment_rows"),"sample_population_rows":selection.get("sample_population_rows"),
            "sample_unrepresented_rows":selection.get("sample_unrepresented_rows"),
            "boundary_probe_rows":selection.get("boundary_probe_rows",0),"boundary_probes":selection.get("boundary_probes",[]),
            "sample_windows":windows,"calibrated_rows":selection["calibrated_rows"],"rounds":selection["rounds"],
            "time_budget_reached":selection["time_budget_reached"],"fresh_mask_checks":check["heldout_mask_checks"],
            "timing_order_audit":order,"spatial_sample":spatial_sample(case,selection)}


def summarize_case(case,directory,configuration):
    result=case["native_result"];m=result["measurements"]
    if case["status"]!="completed" or result["status"]!="completed" or not m["exact"]:raise ValueError("incomplete native case")
    if json.loads((directory/"native/result.json").read_text(encoding="utf-8"))!=result:
        raise ValueError("case and native result copies disagree")
    verify_native_artifacts(result,directory/"native")
    candidates={c["backend"]:c for c in m["candidates"]}
    if set(candidates)!=set(RAW+PORTABLE+GENERATED):raise ValueError("native candidate inventory changed")
    for candidate in candidates.values():validate_samples(candidate,m["repeat"])
    order=validate_orders(candidates,m["timing_order"],m["timing_cycle_rows"],True)
    if m["fresh_mask_checks"]!=len(candidates)*m["repeat"] or m["complete_timing_cycles"]!=order["complete_cycles"]:
        raise ValueError("native timing check/cycle count mismatch")
    starts=(0,1,7,31,63,64,255,case["rows"]-65 if case["rows"]>65 else case["rows"],case["rows"])
    if m["range_mask_checks"]!=len(candidates)*4*sum(first<=case["rows"] for first in starts):
        raise ValueError("independent native range matrix incomplete")
    if result["inputs"]["raw_sha256"]!=case["raw_sha256"] or result["inputs"]["packed_sha256"]!=case["packed_sha256"]:
        raise ValueError("native/case input hashes mismatch")
    if case["data_retained"] and (digest(directory/"input.i32")!=case["raw_sha256"] or digest(directory/"input.sfp")!=case["packed_sha256"]):
        raise ValueError("retained data hashes mismatch")
    for name in (*PREFIX_GENERATED,*PORTABLE[:3]):audit_diagnostic(name,candidates[name]["prefix_diagnostic"],case["rows"])
    diagnostics={name:candidates[name]["prefix_diagnostic"] for name in (*PREFIX_GENERATED,*PORTABLE[:3])}
    # Fixed-depth root bounds must agree across semantic-preserving changes.
    for key in ("ambiguous_rows","certified_rows","dirty_words"):
        if len({d[key] for d in diagnostics.values()})!=1:raise ValueError("matched-depth root uncertainty differs: "+key)
    for key in ("vector_batches","fallback_batches","scalar_batches","scalar_fallback_batches","physical_refined_rows"):
        if len({diagnostics[name][key] for name in PREFIX_GENERATED})!=1:raise ValueError("matched generated branch geometry differs: "+key)
    for variant in result["variants"]:
        if "prefix" in variant and variant["emission"]["encoded_prefix_bits"]!=12:raise ValueError("prefix depth changed")
    if result["frozen_support_library"]["identity"]["namespace_only_transform"] not in {"-Dsuffice=suffice_v04","/Dsuffice=suffice_v04"}:
        raise ValueError("unexpected frozen source transform")
    median=lambda name:candidates[name]["median_ms"]
    best_raw=min(RAW,key=median)
    generated_ratios={"frozen_frontier_over_residual":median(GENERATED[0])/median(GENERATED[3]),
                      "current_frontier_over_residual":median(GENERATED[2])/median(GENERATED[3]),
                      "current_eager_over_residual":median(GENERATED[1])/median(GENERATED[3]),
                      "best_raw_over_residual":median(best_raw)/median(GENERATED[3])}
    portable_ratios={"frozen_frontier_over_hybrid":median(PORTABLE[0])/median(PORTABLE[2]),
                     "current_sparse_over_hybrid":median(PORTABLE[1])/median(PORTABLE[2]),
                     "eager_wordcode_over_hybrid":median(PORTABLE[3])/median(PORTABLE[2]),
                     "best_raw_over_hybrid":median(best_raw)/median(PORTABLE[2])}
    selections=[audit_selection(run,case) for run in case.get("selection_runs",[])]
    expected_seeds=configuration["selection_seeds"] if case["name"] in SEED_SUBSET else configuration["selection_seeds"][:1]
    wanted=set() if configuration["skip_selection"] else {(p,s) for p in POLICIES for s in expected_seeds}
    if len(selections)!=len(wanted) or {(r["policy"],r["seed"]) for r in selections}!=wanted:
        raise ValueError("selection policy/seed inventory incomplete")
    for seed in expected_seeds if selections else []:
        current=next(r for r in selections if r["policy"]=="current-legacy-endpoints" and r["seed"]==seed)
        frozen=next(r for r in selections if r["policy"]=="frozen04-legacy-endpoints" and r["seed"]==seed)
        geometry=lambda run:[(w["first_row"],w["row_count"]) for w in run["sample_windows"]]
        if geometry(current)!=geometry(frozen):raise ValueError("explicit LegacyEndpoints does not reproduce frozen timed-window geometry")
    return {"case":case["name"],"family":case["family"],"pattern":case["pattern"],"constant_control":case["constant_control"],
            "rows":case["rows"],"channels":case["channels"],"accepted":m["accepted"],"mask_checksum":m["mask_checksum"],
            "best_raw_backend":best_raw,"best_raw_ms":median(best_raw),"generated_ratios":generated_ratios,"portable_ratios":portable_ratios,
            "named_layout_ratios":{"aos_scalar_over_soa_scalar":median(RAW[0])/median(RAW[2]),
                                   "aos_simd_over_soa_simd":median(RAW[1])/median(RAW[3])},
            "candidates":m["candidates"],"diagnostics":diagnostics,"selection_runs":selections,"timing_order_audit":order,
            "fresh_mask_checks":m["fresh_mask_checks"],"range_mask_checks":m["range_mask_checks"],
            "setup":{"input_validation_ms":m["validation_ms"],"plan_ms":m["plan_ms"],"oracle_ms":m["oracle_ms"],
                     "aos_copy_ms":m["aos_copy_ms"],"soa_transpose_ms":m["soa_transpose_ms"],
                     "copy_bytes_each":m["aos_copy_bytes"],"generated_prepare_ms":m["generated_prepare_ms"],"core_prepare_ms":m["core_prepare_ms"],
                     "emission_ms":{v["key"]:v["emission_process_wall_ms"] for v in result["variants"]},
                     "wrapper_compilation_ms":{v["key"]:v["compilation"]["wall_ms"] for v in result["variants"]},
                     "frozen_wrapper_compile_ms":result["frozen_portable_compilation"]["wall_ms"],
                     "driver_compile_ms":result["driver_compilation"]["wall_ms"],"link_ms":result["link"]["wall_ms"],
                     "support_this_invocation_ms":result["shared_support_library"]["this_invocation_build_ms"],
                     "frozen_support_this_invocation_ms":result["frozen_support_library"]["this_invocation_build_ms"]},
            "emitted_metadata":{v["key"]:v["emission"] for v in result["variants"]},
            "spatial_input":case.get("generation") if case["pattern"].startswith("spatial_") else None}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix",type=Path,required=True);parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();matrix=args.matrix.resolve()
    suite=json.loads((matrix/"suite.json").read_text(encoding="utf-8"))
    if suite.get("schema_version")!=5 or suite.get("status")!="completed":raise ValueError("requires completed schema5 matrix")
    rows=[summarize_case(case,matrix/case["name"],suite["configuration"]) for case in suite["cases"]]
    ordinary=[r for r in rows if not r["constant_control"] and r["pattern"]!="prefix_stress" and not r["pattern"].startswith("spatial_")]
    stress=[r for r in rows if r["pattern"]=="prefix_stress"]
    spatial={r["pattern"]:r for r in rows if r["pattern"].startswith("spatial_")}
    pair=None
    if set(spatial)=={"spatial_scattered","spatial_clustered"}:
        scattered,clustered=spatial["spatial_scattered"],spatial["spatial_clustered"]
        if scattered["spatial_input"]["row_multiset_fingerprint"]!=clustered["spatial_input"]["row_multiset_fingerprint"] or scattered["accepted"]!=clustered["accepted"]:
            raise ValueError("spatial pair is not a matched multiset/count construction")
        unknown=scattered["rows"]//256
        for pattern,row in spatial.items():
            for name,d in row["diagnostics"].items():
                if d["ambiguous_rows"]!=unknown:raise ValueError("spatial unknown count disagrees with construction")
                expected_dirty=row["rows"]//256 if pattern=="spatial_scattered" else (unknown+63)//64
                if d["dirty_words"]!=expected_dirty:raise ValueError("spatial64dirtywords disagree with construction")
                if name in PREFIX_GENERATED and d["vector_batches"]:
                    expected=row["rows"]//256 if pattern=="spatial_scattered" else (unknown+255)//256
                    if d["fallback_batches"]!=expected:raise ValueError("spatial256batch geometry disagrees with construction")
        pair={"declared_row_multiset_identical":True,"accepted_counts_identical":True,"uncertain_rows":unknown,
              "uncertain_fraction":unknown/scattered["rows"],"diagnostics":{p:r["diagnostics"] for p,r in spatial.items()},
              "independent_sort_of_raw_rows_performed":False}
    grouped=defaultdict(list)
    for row in rows:
        for run in row["selection_runs"]:grouped[run["policy"]].append(run)
    policy_summary={policy:{"runs":len(runs),"selected_counts":dict(Counter(r["selected"] for r in runs)),
                           "selected_fixed_regret":extent(r["selected_fixed_regret"] for r in runs),
                           "actual_instance_regret":extent(r["actual_instance_regret"] for r in runs),
                           "prepare_ms":extent(r["prepare_ms"] for r in runs)} for policy,runs in grouped.items()}
    counts={"cases":len(rows),"ordinary_nonconstant_cases":len(ordinary),"prefix_stress_cases":len(stress),
            "spatial_cases":len(spatial),"constant_controls":sum(r["constant_control"] for r in rows),
            "native_fresh_mask_checks":sum(r["fresh_mask_checks"] for r in rows),"native_range_mask_checks":sum(r["range_mask_checks"] for r in rows),
            "selection_runs":sum(len(r["selection_runs"]) for r in rows),
            "selection_fresh_mask_checks":sum(s["fresh_mask_checks"] for r in rows for s in r["selection_runs"]),
            "native_complete_cycles":sum(r["timing_order_audit"]["complete_cycles"] for r in rows),
            "selection_complete_cycles":sum(s["timing_order_audit"]["complete_cycles"] for r in rows for s in r["selection_runs"])}
    summary={"schema_version":5,"suite_sha256":digest(matrix/"suite.json"),"binaries":suite["binaries"],"configuration":suite["configuration"],
             "counts":counts,"ranges":{},"selection_policy_summary":policy_summary,"spatial_pair":pair,"cases":rows}
    for group_name,group in (("ordinary",ordinary),("stress",stress)):
        for category in ("generated_ratios","portable_ratios"):
            for key in group[0][category] if group else []:
                summary["ranges"][group_name+"_"+key]=extent(r[category][key] for r in group)
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    write_json(output/"SUMMARY.json",summary)
    lines=["# Suffice0.5 matched-depth measurements","",
           "Native ratios divide fresh medians from one process per case; greater than1 favors the denominator. "
           "Every comparison uses fixed prefix depth12 and pressure scheduling. No result-dependent depth or backend is installed as a policy.","",
           "The frozen portable library uses unchanged0.4 sources compiled with the same compiler/flags and only the namespace renamed. "
           "The current forced SparseWords control isolates repair policy from other version changes. "
           "All cases finish compilation before any matrix measurement begins.","",
           "## Portable repair policy","",
           "| Case | Frozen0.4 ms | Current sparse ms | Current hybrid ms | Eager wordcode ms | Frozen / hybrid | Sparse / hybrid | Wordcode / hybrid |",
           "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        c={v["backend"]:v for v in row["candidates"]};p=row["portable_ratios"]
        lines.append(f'| {row["case"]} | '+" | ".join(f'{c[n]["median_ms"]:.6f}' for n in PORTABLE)+f' | {p["frozen_frontier_over_hybrid"]:.3f}x | {p["current_sparse_over_hybrid"]:.3f}x | {p["eager_wordcode_over_hybrid"]:.3f}x |')
    lines += ["","## Generated suffix reuse","",
              "| Case | Frozen frontier ms | Current eager ms | Current frontier ms | Residual ms | Frozen / residual | Current frontier / residual | Best raw / residual |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        c={v["backend"]:v for v in row["candidates"]};p=row["generated_ratios"]
        lines.append(f'| {row["case"]} | '+" | ".join(f'{c[n]["median_ms"]:.6f}' for n in GENERATED)+f' | {p["frozen_frontier_over_residual"]:.3f}x | {p["current_frontier_over_residual"]:.3f}x | {p["best_raw_over_residual"]:.3f}x |')
    lines += ["","## Six raw controls and matched layout ratios","",
              "Best raw is selected only as a reported control envelope. AoS/SoA ratios below compare matching scalar and explicit-SIMD generated algorithms; "
              "the compiler may autovectorize the scalar variant.","",
              "| Case | Best raw | Best raw ms | AoS scalar / SoA scalar | AoS SIMD / SoA SIMD | SoA transpose ms |",
              "| --- | --- | ---: | ---: | ---: | ---: |"]
    for row in rows:
        r=row["named_layout_ratios"]
        lines.append(f'| {row["case"]} | {row["best_raw_backend"]} | {row["best_raw_ms"]:.6f} | {r["aos_scalar_over_soa_scalar"]:.3f}x | {r["aos_simd_over_soa_simd"]:.3f}x | {row["setup"]["soa_transpose_ms"]:.3f} |')
    lines += ["","## Logical uncertainty and physical repair","",
              "Generated counters reproduce exact branch geometry outside timers. They count actual64/256execution widths, not DRAM transfers. "
              "Portable hybrid can repair known lanes inside a dense tile; only uncertain output lanes are merged.","",
              "| Case | Uncertain rows | Dirty64words | Generated dirty256batches | Generated scalar repair batches | Hybrid dense tiles | Hybrid physical rows | Sparse physical rows | Hybrid scratch bytes |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        d=row["diagnostics"];g=d[GENERATED[3]];h=d[PORTABLE[2]];s=d[PORTABLE[1]]
        lines.append(f'| {row["case"]} | {g["ambiguous_rows"]:,} | {g["dirty_words"]:,} | {g["fallback_batches"]:,} | {g["scalar_fallback_batches"]:,} | {h["dense_tiles"]:,} | {h["physical_refined_rows"]:,} | {s["physical_refined_rows"]:,} | {h["scratch_bytes"]:,} |')
    if policy_summary:
        lines += ["","## Selection policy and seed observations","",
                  "These are separate CLI processes on paired identical inputs. Each regret uses fresh fixed controls in that same CLI process. "
                  "Generated AOT kernels are outside the selector. Current Representative versus current LegacyEndpoints changes only sampling policy; "
                  "the frozen CLI also retains the older portable implementation. Three seeds on a declared subset are sensitivity observations, not a statistical guarantee.","",
                  "| Case | Seed | Policy | Selected | Best eligible fixed | Selected-fixed regret | Actual-instance regret | Prepare ms | Work rows / budget |",
                  "| --- | ---: | --- | --- | --- | ---: | ---: | ---: | ---: |"]
        for row in rows:
            for s in sorted(row["selection_runs"],key=lambda x:(x["seed"],x["policy"])):
                lines.append(f'| {row["case"]} | {s["seed"]} | {s["policy"]} | {s["selected"]} | {s["best_eligible_fixed"]} | {s["selected_fixed_regret"]:.3f}x | {s["actual_instance_regret"]:.3f}x | {s["prepare_ms"]:.3f} | {s["work_rows"]:,} / {s["work_budget"]:,} |')
        lines += ["","### Spatial sample composition","",
                  "The timed sample fraction is calculated from the specified input construction. Correctness probes do not contribute timing scores. "
                  "Weighting uses declared stratum population weights. A sample may miss a rare cluster; no sampling procedure here guarantees detection.","",
                  "| Case | Seed | Policy | Global unknown | Unweighted sample unknown | Weighted sample unknown | Timed rows | Probe rows |",
                  "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |"]
        for row in rows:
            for s in row["selection_runs"]:
                if s["spatial_sample"]:
                    p=s["spatial_sample"]
                    lines.append(f'| {row["case"]} | {s["seed"]} | {s["policy"]} | {p["global_unknown_fraction"]:.6%} | {p["sample_unknown_fraction"]:.6%} | {p["weighted_sample_unknown_fraction"]:.6%} | {s["calibrated_rows"]:,} | {s["boundary_probe_rows"]:,} |')
    lines += ["","## Setup costs and audit scope","",
              "Validation, original-IR oracle, raw copy, SoA transpose, per-instance preparation, emission, wrapper compilation, driver compilation, linking, "
              "and current/frozen support archives are retained as separate fields in SUMMARY.json and each native result. Common research costs overlap; "
              "summing them does not establish deployment break-even. Frozen portable preparation includes its own mapping, validation, and plan build.","",
              f'{counts["native_fresh_mask_checks"]:,} fresh native complete masks, {counts["native_range_mask_checks"]:,} independent native range masks, '
              f'and {counts["selection_fresh_mask_checks"]:,} fresh selection/core masks were checked. '
              "This audit recomputes medians and regret, checks complete Williams-cycle position/predecessor balance, source/header/object/binary identities, "
              "input identities, root bounds, repair accounting, legacy window equivalence, and calibration budgets.","",
              "Constants remain visible and are excluded from gain aggregates. Adversarial prefix and spatial cases remain separate from ordinary distributions. "
              "All candidates and raw layouts coexist in the harness. Scratch values describe prepared work buffers; process RSS includes research controls. "
              "These warm synthetic results do not establish Windows, application-wide, arbitrary-AI, external-engine, or hardware-optimal speedups.",""]
    (output/"TABLES.md").write_text("\n".join(lines),encoding="utf-8")
    print(json.dumps(counts,indent=2))


if __name__=="__main__":main()
