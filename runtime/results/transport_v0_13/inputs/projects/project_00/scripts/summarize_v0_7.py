#!/usr/bin/env python3
"""Audit the fixed physical-scheduling matrix and actual bounded selections."""
from __future__ import annotations

import argparse
from collections import Counter
import datetime
from fractions import Fraction
import json
import math
from pathlib import Path
import statistics

from benchmark_v0_2 import digest, write_json
from benchmark_v0_7 import (CALIBRATION, CANDIDATES, PORTFOLIO_ELIGIBLE, PORTFOLIO_LABEL,
                            PORTFOLIO_NAMES, PREFIX, PROOF_BUDGETS, RAW,
                            header_metadata, verify_native_artifacts)
from codegen_support import ROOT
from summarize_v0_4 import extent, validate_orders, validate_samples
from summarize_v0_5 import audit_diagnostic


def require(condition, message):
    if not condition:
        raise ValueError(message)


def same_number(actual, expected, message):
    require(math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-9), message)


def audit_available_sources(suite):
    inventories = {
        "current_sources": {str(ROOT/p): h for p, h in suite["provenance"]["inputs"].items()},
        "frozen_sources": {str(ROOT/"baseline/v0_6"/p): h for p, h in suite["frozen_sources"].items()},
        "generating_binaries": {v["path"]: v["sha256"] for v in suite["binaries"].values()},
        "frozen_full_release": {str(ROOT/suite["frozen_release"]["path"]): suite["frozen_release"]["sha256"]},
    }
    compiler = suite["provenance"]["native_compiler"]
    inventories["compiler_binary"] = {compiler["executable"]: compiler["compiler_sha256"]}
    records = {}
    for label, inventory in inventories.items():
        checked, unavailable = [], []
        for name, expected in inventory.items():
            path = Path(name)
            if not path.is_file():
                unavailable.append(name)
                continue
            require(digest(path) == expected, "available provenance artifact changed: " + name)
            checked.append(name)
        records[label] = {"matched": len(checked), "unavailable": unavailable}
    return records


def audit_portfolio(m, rows, candidates):
    """Independently reconstruct cycles, work, margin rule, and reuse decision."""
    p = m["portfolio_calibration"]
    opts = p["options"]
    require(p["candidate_count"] == 4 and p["incumbent_index"] == 0, "portfolio inventory/fallback changed")
    require(p["candidate_names"] == list(PORTFOLIO_NAMES), "portfolio names/order changed")
    for key in ("cycles", "minimum_cycles", "warmups_per_candidate", "minimum_improvement_basis_points",
                "max_elapsed_nanoseconds", "order_seed"):
        require(opts[key] == CALIBRATION[key], "fixed calibration option changed: " + key)
    require(opts["max_row_evaluations"] == CALIBRATION["max_row_evaluations_per_input_row"] * rows,
            "fixed calibration row budget changed")
    require(opts["expected_remaining_calls"] == m["repeat"], "reuse horizon differs from actual fresh rounds")
    require(p["first"] == 0 and p["count"] == rows, "calibration did not use the declared full input")
    require(m["portfolio_reference_mask_checksum"] == m["mask_checksum"] and
            m["portfolio_reference_accepted"] == m["accepted"], "calibration reference differs from original IR")
    require(m["post_calibration_samples_are_fresh"] and m["calibration_and_measurement_input_same"],
            "missing fresh, same-input selection scope")
    ncycles = p["completed_cycles"]
    require(0 <= ncycles <= opts["cycles"] and len(p["cycles"]) == ncycles,
            "invalid complete calibration cycle count")
    require(len(p["candidates"]) == 4, "calibration candidate measurement count changed")
    orders = []
    for cycle in p["cycles"]:
        require(cycle["round_count"] == 4 and len(cycle["orders"]) == 4,
                "partial calibration cycle was retained")
        validate_orders(range(4), cycle["orders"], 4, True)
        require(len(cycle["median_nanoseconds"]) == 4, "missing calibration cycle medians")
        orders.extend(cycle["orders"])
    aggregate = []
    for index, c in enumerate(p["candidates"]):
        require(c["index"] == index, "calibration candidate index changed")
        require(c["warmup_calls"] == opts["warmups_per_candidate"], "calibration warmup count changed")
        require(c["timed_samples"] == 4*ncycles and c["calls"] == c["warmup_calls"] + c["timed_samples"],
                "calibration call accounting mismatch")
        require(c["row_evaluations"] == c["calls"]*rows, "calibration candidate row work mismatch")
        require(c["cycle_samples"] == [4]*ncycles and len(c["sample_nanoseconds"]) == ncycles and
                len(c["cycle_median_nanoseconds"]) == ncycles, "calibration sample cycle inventory mismatch")
        retained = []
        for cycle, samples in enumerate(c["sample_nanoseconds"]):
            require(len(samples) == 4 and all(isinstance(x, int) and x >= 0 for x in samples),
                    "invalid calibration timing sample")
            median = statistics.median(samples)
            same_number(c["cycle_median_nanoseconds"][cycle], median, "calibration sample/median mismatch")
            same_number(p["cycles"][cycle]["median_nanoseconds"][index], median,
                        "calibration cycle/candidate median mismatch")
            retained.extend(samples)
        require(c["timed_nanoseconds"] == sum(retained), "calibration timed sum differs from samples")
        require(isinstance(c["warmup_nanoseconds"], int) and c["warmup_nanoseconds"] >= 0,
                "invalid warmup duration")
        aggregate.append(statistics.median(c["cycle_median_nanoseconds"]) if ncycles else 0)
    require(p["total_calls"] == sum(c["calls"] for c in p["candidates"]), "calibration total calls mismatch")
    require(p["row_evaluations"] == p["total_calls"]*rows <= opts["max_row_evaluations"],
            "calibration hard row-work budget exceeded")
    require(p["elapsed_nanoseconds"] >= sum(c["timed_nanoseconds"]+c["warmup_nanoseconds"] for c in p["candidates"]),
            "calibration elapsed time smaller than its timed work")
    require(m["portfolio_calibration_wall_ms"] * 1000000 >= p["elapsed_nanoseconds"],
            "external calibration wall time smaller than internal elapsed time")
    if p["stop_reason"] == "completed":
        require(ncycles == opts["cycles"] and p["elapsed_nanoseconds"] < opts["max_elapsed_nanoseconds"],
                "completed calibration stop reason inconsistent with budget")
    elif p["stop_reason"] == "time_budget":
        require(p["elapsed_nanoseconds"] >= opts["max_elapsed_nanoseconds"], "time-budget stop precedes budget")
    else:
        raise ValueError("unexpected calibration stop reason under fixed sufficient row budget")
    proposed = 0
    reason = "insufficient_cycles" if ncycles < opts["minimum_cycles"] else "no_consistent_improvement"
    saving, break_even, selected = 0, 0, 0
    if ncycles >= opts["minimum_cycles"]:
        consistent = []
        for index in range(1, 4):
            # The retained integer/half-integer medians permit an exact rational
            # comparison instead of repeating the C++ floating point expression.
            if all(0 < cycle["median_nanoseconds"][index] < cycle["median_nanoseconds"][0] and
                   Fraction(cycle["median_nanoseconds"][index])*10000 <=
                   Fraction(cycle["median_nanoseconds"][0])*(10000-opts["minimum_improvement_basis_points"])
                   for cycle in p["cycles"]):
                consistent.append(index)
        if consistent:
            proposed = min(consistent, key=lambda i: (aggregate[i], i))
            saving = aggregate[0] - aggregate[proposed]
            break_even = math.ceil(Fraction(p["elapsed_nanoseconds"])/Fraction(saving))
            reason = "selected" if opts["expected_remaining_calls"] >= break_even else "not_amortized"
            if reason == "selected":
                selected = proposed
        same_number(p["incumbent_median_nanoseconds"], aggregate[0], "incumbent calibration estimate differs")
        same_number(p["proposed_median_nanoseconds"], aggregate[proposed], "proposed calibration estimate differs")
    else:
        same_number(p["incumbent_median_nanoseconds"], 0, "insufficient-cycle incumbent estimate should be zero")
        same_number(p["proposed_median_nanoseconds"], 0, "insufficient-cycle proposal estimate should be zero")
    require(p["proposed_index"] == proposed and p["selected_index"] == selected and p["reason"] == reason,
            "retained selector decision differs from independent margin/reuse audit")
    require(p["changed"] == (selected != 0) and p["selected_name"] == PORTFOLIO_NAMES[selected],
            "actual selected kernel identity differs from decision")
    require(not p["break_even_saturated"] and p["estimated_break_even_calls"] == break_even,
            "calibration break-even estimate differs")
    same_number(p["estimated_saving_nanoseconds"], saving, "estimated calibration saving differs")
    median = lambda name: candidates[name]["median_ms"]
    best = min(PORTFOLIO_ELIGIBLE, key=median)
    actual = median(PORTFOLIO_LABEL)
    selected_control = PORTFOLIO_ELIGIBLE[selected]
    calibration_ms = m["portfolio_calibration_wall_ms"]
    return {
        "report": p, "decision_independently_reconstructed": True,
        "calibration_order_audit": validate_orders(range(4), orders, 4, True),
        "calibration_wall_ms": calibration_ms,
        "soft_time_budget_overshoot_ms": max(0, p["elapsed_nanoseconds"]-opts["max_elapsed_nanoseconds"])/1000000,
        "selected_control": selected_control, "selected_control_fresh_ms": median(selected_control),
        "actual_portfolio_fresh_ms": actual, "best_eligible_fresh_control": best,
        "best_eligible_fresh_ms": median(best),
        "actual_portfolio_over_best_eligible": actual/median(best),
        "selected_control_over_best_eligible": median(selected_control)/median(best),
        "actual_portfolio_over_corresponding_standalone": actual/median(selected_control),
        "calibration_amortized_ms_per_expected_call": calibration_ms/opts["expected_remaining_calls"],
        "actual_portfolio_plus_calibration_ms_per_expected_call": actual+calibration_ms/opts["expected_remaining_calls"],
        "same_input_not_heldout": True,
        "regret_scope": "Fresh observed ratios, not policy decisions or out-of-distribution predictions. Portfolio and standalone controls are separately compiled code; the actual-instance ratio includes dispatch and code-layout effects.",
    }


def summarize_case(case, directory):
    result = case["native_result"]
    m = result["measurements"]
    require(case["status"] == result["status"] == "completed" and m["exact"], "requires complete exact native case")
    require(json.loads((directory/"native/result.json").read_text()) == result, "case/native result copies disagree")
    require(json.loads((directory/"native/measurements.json").read_text()) == m, "native measurement copies disagree")
    require(json.loads((directory/"case.json").read_text()) == case, "suite/case copies disagree")
    verify_native_artifacts(result, directory/"native")
    candidates = {c["backend"]: c for c in m["candidates"]}
    require(len(candidates) == len(CANDIDATES) and set(candidates) == set(CANDIDATES), "candidate inventory changed")
    for c in candidates.values():
        validate_samples(c, m["repeat"])
    order = validate_orders(candidates, m["timing_order"], m["timing_cycle_rows"], True)
    require(len(m["timing_order"]) == m["repeat"], "sample/order round inventory mismatch")
    require(m["fresh_mask_checks"] == len(candidates)*m["repeat"], "fresh full-mask inventory mismatch")
    require(m["complete_timing_cycles"] == order["complete_cycles"], "native cycle inventory mismatch")
    require(m["rows"] == case["rows"] and m["channels"] == case["channels"], "native dimensions changed")
    starts = (0, 1, 7, 31, 63, 64, 255, case["rows"]-65 if case["rows"] > 65 else case["rows"], case["rows"])
    expected_ranges = len(candidates)*4*sum(x <= case["rows"] for x in starts)
    require(m["range_mask_checks"] == expected_ranges, "native range matrix incomplete")
    for key in ("raw_sha256", "packed_sha256", "program_sha256"):
        require(result["inputs"][key] == case[key], "case/native input identity mismatch: " + key)
    require(digest(directory/"program.suf") == case["program_sha256"], "retained program changed")
    if case["data_retained"]:
        require(digest(directory/"input.i32") == case["raw_sha256"] and digest(directory/"input.sfp") == case["packed_sha256"],
                "retained input changed")
    diagnostics = {name: candidates[name]["prefix_diagnostic"] for name in PREFIX}
    for name, diagnostic in diagnostics.items():
        audit_diagnostic(name, diagnostic, case["rows"])
    for key in ("ambiguous_rows", "certified_rows", "dirty_words", "vector_batches", "fallback_batches",
                "scalar_batches", "scalar_fallback_batches", "physical_refined_rows"):
        require(len({v[key] for v in diagnostics.values()}) == 1, "matched bound/branch geometry differs: " + key)
    metadata = {v["key"]: v["header_metadata"] for v in result["variants"]}
    for variant in result["variants"]:
        current = header_metadata(directory/"native"/variant["header"])
        require(current == variant["header_metadata"], "header metadata changed")
        emitted = variant["emission"]
        require(json.loads((directory/"native"/f'emission_{variant["key"]}.json').read_text()) == emitted,
                "retained emission metadata differs")
        for field in ("strategy", "schedule"):
            require(emitted["encoded_"+field] == variant[field] == current["encoded_"+field],
                    "generated/emitted/requested choice mismatch: " + field)
        require(current["encoded_carry_placement"] == variant["placement"], "carry placement changed")
        if "prefix" in variant:
            require(current["encoded_prefix_bits"] == 12, "fixed prefix depth changed")
        for key, value in current.items():
            if key in emitted:
                require(value == emitted[key], "header/CLI metadata mismatch: " + key)
        if variant["placement"] == "recompute":
            require(current["encoded_recompute_carry_emitted"], "requested rematerialization fell back")
        if variant["strategy"] == "guarded-residual":
            require(all(emitted.get(k) == v for k, v in PROOF_BUDGETS.items()), "fixed proof budgets changed")
            initial, remaining = current["encoded_residual_initial_carries"], current["encoded_residual_carries"]
            accepted = current["encoded_proof_accepted_rewrites"]
            require(0 <= remaining <= initial and accepted <= current["encoded_proof_attempts"] and
                    accepted <= initial-remaining, "care proof/carry accounting mismatch")
            require(current["encoded_residual_scope"] in {"all-lanes", "unresolved-lanes"}, "unknown residual scope")
            require(current["encoded_residual_exact_all_lanes"] == (current["encoded_residual_scope"] == "all-lanes"),
                    "residual exactness scope mismatch")
    semantic_fields = ("encoded_prefix_nodes", "encoded_prefix_planes", "encoded_residual_nodes", "encoded_residual_planes",
                       "encoded_residual_carries", "encoded_residual_initial_carries", "encoded_residual_scope",
                       "encoded_proof_attempts", "encoded_proof_accepted_rewrites", "encoded_proof_bdd_nodes",
                       "encoded_proof_steps", "encoded_proof_exhausted", "encoded_proof_graph_budget_rejections")
    for semantic in ("residual", "care"):
        keys = [f"{semantic}_{schedule}_{placement}" for schedule in ("pressure", "bounds_first")
                for placement in ("guarded", "recompute")]
        for key in semantic_fields:
            require(len({metadata[name][key] for name in keys}) == 1, "physical choices changed semantic plan: " + key)
    portfolio = result["portfolio"]
    pe = portfolio["emission"]
    require(json.loads((directory/"native/emission_portfolio.json").read_text()) == pe, "portfolio emission changed")
    require(pe["candidate_count"] == 4 and pe["fallback_index"] == 0 and pe["portfolio_emitted"] and
            pe["runtime_calibration_performed"] is False, "portfolio generation contract changed")
    require([v["name"] for v in pe["variants"]] == list(PORTFOLIO_NAMES), "portfolio emitted inventory changed")
    for index, emitted in enumerate(pe["variants"]):
        standalone = next(v for v in result["variants"] if v["label"] == PORTFOLIO_ELIGIBLE[index])
        require(emitted["index"] == index, "portfolio emitted candidate index changed")
        for key in semantic_fields:
            require(emitted[key] == standalone["emission"][key], "portfolio/standalone semantic plan differs: " + key)
        for key in ("encoded_strategy", "encoded_schedule", "encoded_carry_placement"):
            require(emitted[key] == standalone["emission"][key], "portfolio/standalone physical choice differs: " + key)
    selection = audit_portfolio(m, case["rows"], candidates)
    median = lambda name: candidates[name]["median_ms"]
    best_raw = min(RAW, key=median)
    pairs = {}
    for semantic in ("residual", "care"):
        base = f"v07-{semantic}-pressure-guarded"
        combined = f"v07-{semantic}-bounds-first-recompute"
        pairs.update({
            semantic+"_schedule_only": (base, f"v07-{semantic}-bounds-first-guarded"),
            semantic+"_recompute_only": (base, f"v07-{semantic}-pressure-recompute"),
            semantic+"_combined": (base, combined),
            semantic+"_frozen_over_current_control": (f"v06-{semantic}-pressure-guarded", base),
            semantic+"_frozen_over_combined": (f"v06-{semantic}-pressure-guarded", combined),
        })
    pairs.update({"care_at_control": ("v07-residual-pressure-guarded", "v07-care-pressure-guarded"),
                  "care_at_combined": ("v07-residual-bounds-first-recompute", "v07-care-bounds-first-recompute"),
                  "best_raw_over_combined_care": (best_raw, "v07-care-bounds-first-recompute"),
                  "frontier_over_combined_care": ("v07-frontier", "v07-care-bounds-first-recompute"),
                  "eager_over_combined_care": ("v07-eager", "v07-care-bounds-first-recompute"),
                  "wordcode_over_combined_care": ("portable-wordcode", "v07-care-bounds-first-recompute"),
                  "fallback_over_actual_portfolio": (PORTFOLIO_ELIGIBLE[0], PORTFOLIO_LABEL),
                  "best_raw_over_actual_portfolio": (best_raw, PORTFOLIO_LABEL)})
    frozen_matches = {}
    for semantic in ("residual", "care"):
        frozen_matches[semantic] = {key: metadata["frozen_"+semantic+"_guarded"].get(key) ==
                                  metadata[semantic+"_pressure_guarded"].get(key) for key in semantic_fields}
    return {
        "case": case["name"], "family": case["family"], "category": case["category"], "pattern": case["pattern"],
        "measurement_purpose": m["measurement_purpose"],
        "rows": case["rows"], "channels": case["channels"], "accepted": m["accepted"], "mask_checksum": m["mask_checksum"],
        "best_raw_backend": best_raw, "best_raw_ms": median(best_raw),
        "ratios": {name: median(a)/median(b) for name, (a, b) in pairs.items()},
        "ratio_operands": {name: {"numerator": a, "denominator": b} for name, (a, b) in pairs.items()},
        "candidates": m["candidates"], "diagnostics": diagnostics, "header_metadata": metadata,
        "emitted_metadata": {v["key"]: v["emission"] for v in result["variants"]},
        "portfolio_emitted_metadata": pe, "portfolio": selection,
        "frozen_current_semantic_plan_matches": frozen_matches,
        "timing_order_audit": order, "fresh_mask_checks": m["fresh_mask_checks"], "range_mask_checks": m["range_mask_checks"],
        "setup": {"packing_process_ms": case["packing"]["process_wall_ms"], "input_validation_ms": m["validation_ms"],
                  "plan_ms": m["plan_ms"], "oracle_ms": m["oracle_ms"], "aos_copy_ms": m["aos_copy_ms"],
                  "soa_transpose_ms": m["soa_transpose_ms"], "copy_bytes_each": m["aos_copy_bytes"],
                  "generated_prepare_ms": m["generated_prepare_ms"], "core_prepare_ms": m["core_prepare_ms"],
                  "emission_process_ms": {v["key"]: v["emission_process_wall_ms"] for v in (*result["variants"], portfolio)},
                  "generation_ms": {v["key"]: v["emission"]["generation_ms"] for v in (*result["variants"], portfolio)},
                  "wrapper_compile_ms": {v["key"]: v["compilation"]["wall_ms"] for v in (*result["variants"], portfolio)},
                  "driver_compile_ms": result["driver_compilation"]["wall_ms"], "link_ms": result["link"]["wall_ms"],
                  "support_this_invocation_ms": result["shared_support_library"]["this_invocation_build_ms"]},
        "measurement_started_utc": case["measurement_started_utc"], "measurement_completed_utc": case["measurement_completed_utc"],
        "discarded_measurements": case.get("discarded_measurements", []),
    }


def write_tables(output, summary):
    rows = summary["cases"]
    lines = ["# Suffice 0.7 fixed physical-scheduling experiment", "",
             "All time ratios use fresh medians from one native process per input. Above one favors the denominator. "
             "Calibration samples are retained separately. The actual portfolio chooses before any fresh comparison sample.", "",
             "## Physical factors, same semantic residual", "",
             "| Case | All: schedule | All: recompute | All: combined | Care: schedule | Care: recompute | Care: combined |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    keys = tuple(semantic+"_"+factor for semantic in ("residual", "care") for factor in ("schedule_only", "recompute_only", "combined"))
    for row in rows:
        lines.append("| "+row["case"]+" | "+" | ".join(f'{row["ratios"][key]:.4f}x' for key in keys)+" |")
    lines += ["", "Each column divides the current Pressure/guarded control by the named physical variant. "
              "Schedule means BoundsFirst/guarded; recompute means Pressure/recompute; combined means BoundsFirst/recompute. "
              "All-lane and care-reduced residuals form separate semantic groups. Exact operands and every sample appear in SUMMARY.json.", "",
              "## Actual portfolio after calibration", "",
              "| Case | Selected option | Decision | Cycles | Calibration ms | Expected reuse | Estimated break-even | Fresh portfolio ms | Portfolio / best eligible |",
              "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        p = row["portfolio"]; r = p["report"]
        lines.append(f'| {row["case"]} | {r["selected_name"]} | {r["reason"]} | {r["completed_cycles"]} | '
                     f'{p["calibration_wall_ms"]:.3f} | {r["options"]["expected_remaining_calls"]} | '
                     f'{r["estimated_break_even_calls"]} | {p["actual_portfolio_fresh_ms"]:.6f} | '
                     f'{p["actual_portfolio_over_best_eligible"]:.4f}x |')
    lines += ["", "The eligible envelope has four fixed kernels; it is a reporting reference. It never replaces the actual selected kernel. "
              "The final ratio is observed post-selection regret: lower is better, and values below one can reflect different generated code placement or measurement variation. "
              "A zero estimated break-even means no qualifying proposal was available; it does not mean calibration was free. "
              "Calibration and fresh rounds use the same immutable input. They are distinct observations, not held-out data.", "",
              "## Frozen controls and raw controls", "",
              "| Case | Frozen06 all ms | Current all control ms | Frozen06 care ms | Current care control ms | Combined care ms | Frontier ms | Eager ms | Best raw ms |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    display = ("v06-residual-pressure-guarded", "v07-residual-pressure-guarded", "v06-care-pressure-guarded",
               "v07-care-pressure-guarded", "v07-care-bounds-first-recompute", "v07-frontier", "v07-eager")
    for row in rows:
        c = {v["backend"]: v for v in row["candidates"]}
        lines.append("| "+row["case"]+" | "+" | ".join(f'{c[n]["median_ms"]:.6f}' for n in display)+f' | {row["best_raw_ms"]:.6f} |')
    lines += ["", "Best raw is an observed envelope over all four raw AoS/SoA scalar/SIMD controls. All eighteen native medians, "
              "ranges and samples remain in SUMMARY.json, including portable wordcode and every negative outcome. "
              "Frozen headers were emitted by the retained 0.6 CLI, then built using the same current compiler and packed-reader support interface.", "",
              "## Source-level common and cold work", "",
              "| Case | Semantic | Carry words | Control common nodes/live | Combined common nodes/live | Combined cold prefix nodes/live |",
              "| --- | --- | ---: | ---: | ---: | ---: |"]
    for row in rows:
        for semantic in ("residual", "care"):
            a = row["header_metadata"][semantic+"_pressure_guarded"]
            b = row["header_metadata"][semantic+"_bounds_first_recompute"]
            lines.append(f'| {row["case"]} | {semantic} | {b["encoded_residual_carries"]} | '
                         f'{a["encoded_common_nodes"]}/{a["encoded_common_live_values"]} | '
                         f'{b["encoded_common_nodes"]}/{b["encoded_common_live_values"]} | '
                         f'{b["encoded_cold_prefix_nodes"]}/{b["encoded_cold_prefix_live_values"]} |')
    lines += ["", "These are emitted source-graph metrics. They are not measured registers, stack high-water marks, RSS, memory traffic, or speed guarantees. "
              "Recomputation moves prefix-derived carry construction into a repair helper; it can reread prefix planes and increase cold work. "
              "The unchanged suffix helper consumes carry state and suffix planes. The final mask remains exact through the outer must/uncertainty merge.", "",
              "## Audit and scope", "",
              f'{summary["counts"]["fresh_full_masks"]:,} fresh complete masks and {summary["counts"]["range_masks"]:,} range masks were checked against original scalar arithmetic. '
              f'{summary["counts"]["calibration_calls"]:,} additional calibration calls were checked against the same independently prepared reference. '
              "The summary independently reconstructs each decision from raw calibration observations, each median, complete Williams order balance, "
              "row-work accounting, the reuse gate, artifact hashes, metadata, root-bound diagnostics, and input identities.", "",
              "The 250ms calibration time limit is soft and checked between complete cycles. Every completed cycle and any overshoot is retained. "
              "The 52N row-work budget is hard. Oracle creation, generated-header emission, compilation, packing, preparation, copies and transposition "
              "are recorded separately from warm execution. Calibration includes its output poisoning and validation in its total elapsed budget, but those operations are outside candidate timers.", "",
              "The fixed research harness holds all eighteen candidates and two raw copies simultaneously. Its total process memory is not the memory cost of a deployed single kernel. "
              "All generation/compiler times are observed wall times; isolated compiler/proof-engine peak memory is not measured. "
              "No Windows, application-wide AI, external-engine superiority, held-out generalization or universal tenfold claim follows from these warm synthetic measurements.", ""]
    if summary["configuration"]["measurement_purpose"] == "functional_pipeline_pilot":
        lines[0] = "# Suffice 0.7 functional pipeline pilot"
        lines[2:2] = ["**Non-performance evidence.** This tiny-input pilot validates compilation, report serialization, exactness, resume, and audits. Its timings must not support performance claims.", ""]
    (output/"TABLES.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    matrix = args.matrix.resolve(); suite_path = matrix/"suite.json"
    suite = json.loads(suite_path.read_text())
    require(suite["schema_version"] == 7 and suite["status"] == "completed", "requires completed schema7 suite")
    require(suite["configuration"]["proof_budgets"] == PROOF_BUDGETS and
            suite["configuration"]["portfolio_calibration"] == CALIBRATION, "suite fixed budgets changed")
    require(suite["configuration"]["candidate_inventory"] == list(CANDIDATES), "suite candidate inventory changed")
    freshness = audit_available_sources(suite)
    require([c["name"] for c in suite["cases"]] == [d["name"] for d in suite["configuration"]["definitions"]],
            "case definition/order inventory changed")
    rows = [summarize_case(case, matrix/case["name"]) for case in suite["cases"]]
    for row, case in zip(rows, suite["cases"]):
        require(row["measurement_purpose"] == case["native_result"]["measurement_purpose"] ==
                suite["configuration"]["measurement_purpose"], "measurement purpose labels disagree")
        require(datetime.datetime.fromisoformat(suite["all_compilation_completed_utc"]) <=
                datetime.datetime.fromisoformat(row["measurement_started_utc"]), "recorded compilation completion follows measurement start")
        require(case["native_result"]["frozen_release"] == suite["frozen_release"], "case/suite release identity mismatch")
    require(datetime.datetime.fromisoformat(suite["objective_and_workload_frozen_utc"]) <=
            datetime.datetime.fromisoformat(suite["all_compilation_completed_utc"]), "objective/workload freeze follows compilation completion")
    spatial = {r["pattern"]: r for r in rows if r["category"] == "spatial"}
    pair = None
    if set(spatial) == {"spatial_scattered", "spatial_clustered"}:
        audit = json.loads((matrix/"spatial_input_audit.json").read_text())
        require(audit["actual_raw_rows_checked"] and audit["row_multisets_equal"], "missing actual spatial input audit")
        unknown = spatial["spatial_scattered"]["rows"]//256
        require(len({r["accepted"] for r in spatial.values()}) == 1, "spatial accepted counts differ")
        for pattern, row in spatial.items():
            case = next(c for c in suite["cases"] if c["name"] == row["case"])
            require(audit["cases"][pattern]["raw_sha256"] == case["raw_sha256"], "spatial audit/raw identity mismatch")
            for d in row["diagnostics"].values():
                require(d["ambiguous_rows"] == unknown, "spatial uncertainty count differs")
                dirty = row["rows"]//256 if pattern == "spatial_scattered" else (unknown+63)//64
                require(d["dirty_words"] == dirty, "spatial dirty-word count differs")
                if d["vector_batches"]:
                    batches = row["rows"]//256 if pattern == "spatial_scattered" else (unknown+255)//256
                    require(d["fallback_batches"] == batches, "spatial repair batch count differs")
        pair = {"actual_raw_row_multisets_verified_equal": True, "uncertain_rows": unknown,
                "uncertain_fraction": unknown/spatial["spatial_scattered"]["rows"],
                "input_audit_sha256": digest(matrix/"spatial_input_audit.json")}
    counts = {"cases": len(rows), "candidate_count": len(CANDIDATES),
              "categories": dict(Counter(r["category"] for r in rows)),
              "fresh_full_masks": sum(r["fresh_mask_checks"] for r in rows),
              "range_masks": sum(r["range_mask_checks"] for r in rows),
              "complete_native_cycles": sum(r["timing_order_audit"]["complete_cycles"] for r in rows),
              "calibration_calls": sum(r["portfolio"]["report"]["total_calls"] for r in rows),
              "calibration_complete_cycles": sum(r["portfolio"]["report"]["completed_cycles"] for r in rows),
              "portfolio_decisions": dict(Counter(r["portfolio"]["report"]["reason"] for r in rows))}
    ranges = {}
    for category in sorted({r["category"] for r in rows}):
        group = [r for r in rows if r["category"] == category]
        ranges[category] = {key: extent(r["ratios"][key] for r in group) for key in group[0]["ratios"]}
    summary = {"schema_version": 7, "suite_sha256": digest(suite_path), "configuration": suite["configuration"],
               "binaries": suite["binaries"], "frozen_release": suite["frozen_release"],
               "counts": counts, "ranges": ranges, "spatial_pair": pair, "cases": rows,
               "source_freshness_audit": freshness,
               "resource_measurement_scope": {"compiler_wall_time_observed": True, "isolated_compiler_peak_memory_measured": False,
                                              "isolated_proof_engine_peak_memory_measured": False},
               "chronology": {"objective_and_workload_frozen_utc": suite["objective_and_workload_frozen_utc"],
                              "all_compilation_completed_utc": suite["all_compilation_completed_utc"],
                              "last_preflight_completed_utc": suite["last_preflight_completed_utc"],
                              "measurement_sessions": suite["measurement_sessions"]}}
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    write_json(output/"SUMMARY.json", summary)
    write_tables(output, summary)
    print(json.dumps(counts, indent=2))


if __name__ == "__main__":
    main()
