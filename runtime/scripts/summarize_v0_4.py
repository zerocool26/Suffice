#!/usr/bin/env python3
"""Audit and summarize a completed Suffice 0.4 matrix without rerunning timings."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics

from benchmark_v0_2 import digest, write_json


def validate_orders(names, orders, cycle_rows, require_complete=True):
    names = list(names)
    count = len(names)
    if len(set(names)) != count:
        raise ValueError("duplicate candidate name")
    expected_cycle = count if count == 1 or count % 2 == 0 else 2 * count
    if cycle_rows != expected_cycle:
        raise ValueError("wrong Williams cycle length")
    wanted = set(names)
    for row in orders:
        if len(row) != count or set(row) != wanted:
            raise ValueError("timing order is not a candidate permutation")
    complete, remainder = divmod(len(orders), cycle_rows)
    if require_complete and remainder:
        raise ValueError("unexpected incomplete native timing cycle")
    for start in range(0, complete * cycle_rows, cycle_rows):
        position, predecessor = Counter(), Counter()
        for row in orders[start:start + cycle_rows]:
            position.update((column, name) for column, name in enumerate(row))
            predecessor.update(zip(row, row[1:]))
        expected = cycle_rows // count
        for column in range(count):
            for name in names:
                if position[column, name] != expected:
                    raise ValueError("unbalanced candidate positions")
        for before in names:
            for after in names:
                if before != after and predecessor[before, after] != expected:
                    raise ValueError("unbalanced within-round predecessor pairs")
    return {"candidates": count, "rounds": len(orders), "complete_cycles": complete,
            "partial_cycle_rounds": remainder}


def validate_samples(candidate, repeat):
    samples = candidate["samples_ms"]
    if len(samples) != repeat or any(not math.isfinite(value) or value <= 0 for value in samples):
        raise ValueError("invalid fresh timing samples")
    timing = candidate.get("timing", candidate)
    for key, expected in (("median_ms", statistics.median(samples)), ("min_ms", min(samples)), ("max_ms", max(samples))):
        if key in timing and not math.isclose(timing[key], expected, rel_tol=1e-10, abs_tol=1e-12):
            raise ValueError("timing summary does not match retained samples: " + key)


def summarize_case(case, directory):
    result = case["native_result"]
    measurements = result["measurements"]
    if not measurements["exact"]:
        raise ValueError("missing exact native result")
    candidates = {candidate["backend"]: candidate for candidate in measurements["candidates"]}
    for candidate in candidates.values():
        validate_samples(candidate, measurements["repeat"])
    order = validate_orders(candidates, measurements["timing_order"], measurements["timing_cycle_rows"])
    if measurements["fresh_mask_checks"] != len(candidates) * measurements["repeat"]:
        raise ValueError("wrong fresh complete-mask check count")
    if measurements["complete_timing_cycles"] != order["complete_cycles"]:
        raise ValueError("wrong complete-cycle count")
    for variant in result["variants"]:
        header = directory / "native" / variant["header"]
        if not header.is_file() or digest(header) != variant["header_sha256"]:
            raise ValueError("retained generated header hash mismatch: " + str(header))
    if result["inputs"]["raw_sha256"] != case["raw_sha256"] or result["inputs"]["packed_sha256"] != case["packed_sha256"]:
        raise ValueError("input identities disagree across case and native result")
    raw = [candidate for candidate in candidates.values() if candidate["representation"] in {"interleaved", "planar"}]
    best_raw = min(raw, key=lambda candidate: candidate["median_ms"])
    best_aos = min((c for c in raw if c["representation"] == "interleaved"), key=lambda c: c["median_ms"])
    best_soa = min((c for c in raw if c["representation"] == "planar"), key=lambda c: c["median_ms"])
    encoded = [candidate for candidate in candidates.values() if candidate["representation"] == "packed"]
    best_encoded = min(encoded, key=lambda candidate: candidate["median_ms"])
    frozen = candidates["v03-eager"]["median_ms"]
    current = candidates["v04-eager-pressure"]["median_ms"]
    depth12 = candidates["v04-frontier12"]["median_ms"]
    prefix = {}
    for depth in (8, 12, 16):
        candidate = candidates[f"v04-frontier{depth}"]
        diag = candidate["prefix_diagnostic"]
        if not diag["available"]:
            raise ValueError("missing generated frontier bound diagnostic")
        if diag["certified_rows"] + diag["ambiguous_rows"] != diag["bounds_checked_rows"]:
            raise ValueError("prefix diagnostic row accounting mismatch")
        if diag["bounds_checked_rows"] != 256 * diag["vector_batches"]:
            raise ValueError("prefix diagnostic vector accounting mismatch")
        if diag["bounds_checked_rows"] + diag["scalar_or_tail_rows"] != measurements["rows"]:
            raise ValueError("prefix diagnostic complete-row accounting mismatch")
        if not 0 <= diag["fallback_batches"] <= diag["vector_batches"]:
            raise ValueError("invalid prefix fallback count")
        prefix[str(depth)] = {"median_ms": candidate["median_ms"],
                              "frozen_over_frontier": frozen / candidate["median_ms"],
                              "best_raw_over_frontier": best_raw["median_ms"] / candidate["median_ms"],
                              "diagnostic": diag,
                              "full_batch_refinement_rows": 256 * diag["fallback_batches"],
                              "fallback_batch_fraction": diag["fallback_batches"] / diag["vector_batches"] if diag["vector_batches"] else None,
                              "ambiguous_row_fraction": diag["ambiguous_rows"] / diag["bounds_checked_rows"] if diag["bounds_checked_rows"] else None}
    constant = bool(case.get("constant_control"))
    row = {"case": case["name"], "family": case["family"], "pattern": case["pattern"],
           "constant_control": constant, "rows": case["rows"], "channels": case["channels"],
           "accepted": measurements["accepted"], "mask_checksum": measurements["mask_checksum"],
           "best_raw_backend": best_raw["backend"], "best_raw_ms": best_raw["median_ms"],
           "best_aos_backend": best_aos["backend"], "best_aos_ms": best_aos["median_ms"],
           "best_soa_backend": best_soa["backend"], "best_soa_ms": best_soa["median_ms"],
           "aos_over_soa": best_aos["median_ms"] / best_soa["median_ms"],
           "frozen_eager_ms": frozen, "pressure_eager_ms": current, "frontier12_ms": depth12,
           "frozen_over_pressure": frozen / current, "frozen_over_frontier12": frozen / depth12,
           "best_raw_over_frontier12": best_raw["median_ms"] / depth12,
           "best_encoded_backend": best_encoded["backend"], "best_encoded_ms": best_encoded["median_ms"],
           "best_raw_over_best_encoded": best_raw["median_ms"] / best_encoded["median_ms"],
           "prefix": prefix, "candidates": measurements["candidates"], "timing_order_audit": order,
           "fresh_mask_checks": measurements["fresh_mask_checks"], "range_mask_checks": measurements["range_mask_checks"],
           "aos_copy_ms": measurements["aos_copy_ms"], "soa_transpose_ms": measurements["soa_transpose_ms"],
           "raw_copy_bytes_each": measurements["aos_copy_bytes"],
           "wrapper_compilation_ms": {v["key"]: v["compilation"]["wall_ms"] for v in result["variants"]},
           "driver_compilation_ms": result["driver_compilation"]["wall_ms"], "link_ms": result["link"]["wall_ms"]}
    if case["pattern"].startswith("spatial_"):
        row["spatial_input"] = case["generation"]
    core = case.get("core")
    if core:
        if not core["verification"]["exact_match"] or core["verification"]["mask_checksum"] != row["mask_checksum"] or core["verification"]["accepted"] != row["accepted"]:
            raise ValueError("core and native exact outputs disagree")
        fixed = core["fixed"]
        for candidate in fixed:
            validate_samples(candidate, core["repeat"])
        validate_samples(core["adaptive"], core["repeat"])
        core_names = [c["backend"] for c in fixed] + ["adaptive"]
        fresh = core["fresh_execution"]
        # The CLI names the automatic prepared instance separately in its order.
        observed_names = list(fresh["round_order"][0])
        if set(observed_names) != set(core_names):
            possible = [name for name in observed_names if name not in {c["backend"] for c in fixed}]
            if len(possible) != 1:
                raise ValueError("cannot identify adaptive timing-order label")
            core_names = [c["backend"] for c in fixed] + possible
        audited = validate_orders(core_names, fresh["round_order"], fresh["cycle_rows"], require_complete=False)
        selection = core["selection"]
        if selection["total_calibration_row_evaluations"] > selection["calibration_row_budget"]:
            raise ValueError("core requested-row budget exceeded")
        row["core"] = {"selected": core["adaptive"]["backend"],
                       "selected_fixed_regret": core["selection_regret"]["selected_fixed_over_best_eligible_fixed"],
                       "actual_instance_regret": core["selection_regret"]["adaptive_over_best_eligible_fixed"],
                       "preparation_ms": core["preparation"]["adaptive_construct_wall_ms"],
                       "validation_ms": core["preparation"]["validation_ms"],
                       "work_rows": selection["total_calibration_row_evaluations"],
                       "work_budget": selection["calibration_row_budget"],
                       "fresh_mask_checks": core["verification"]["heldout_mask_checks"],
                       "timing_order_audit": audited,
                       "frontier_stats": next((c["stats"] for c in fixed if c["backend"] == "encoded-frontier"), None)}
    return row


def extent(values):
    values = list(values)
    return {"min": min(values), "max": max(values), "median": statistics.median(values)} if values else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    matrix = args.matrix.resolve()
    suite = json.loads((matrix / "suite.json").read_text(encoding="utf-8"))
    if suite.get("schema_version") != 4 or suite.get("status") != "completed":
        raise ValueError("summary requires a complete schema4 matrix")
    rows = [summarize_case(case, matrix / case["name"]) for case in suite["cases"]]
    if any(case["status"] != "completed" for case in suite["cases"]):
        raise ValueError("a final case is incomplete")
    nonconstant = [row for row in rows if not row["constant_control"]]
    ordinary = [row for row in nonconstant if row["pattern"] != "prefix_stress" and not row["pattern"].startswith("spatial_")]
    stress = [row for row in nonconstant if row["pattern"] == "prefix_stress"]
    spatial = [row for row in nonconstant if row["pattern"].startswith("spatial_")]
    spatial_pair = None
    if len(spatial) == 2:
        by_pattern = {row["pattern"]: row for row in spatial}
        scattered, clustered = by_pattern["spatial_scattered"], by_pattern["spatial_clustered"]
        if (scattered["spatial_input"]["row_multiset_fingerprint"] != clustered["spatial_input"]["row_multiset_fingerprint"]
                or scattered["accepted"] != clustered["accepted"] or scattered["rows"] != clustered["rows"]):
            raise ValueError("spatial pair does not preserve row multiset/counts")
        unknown = scattered["spatial_input"]["uncertain_rows"]
        for depth in ("8", "12", "16"):
            for row in spatial:
                diagnostic = row["prefix"][depth]["diagnostic"]
                if diagnostic["vector_batches"]:
                    expected_fallback = row["rows"] // 256 if row["pattern"] == "spatial_scattered" else (unknown + 255) // 256
                    if diagnostic["ambiguous_rows"] != unknown or diagnostic["fallback_batches"] != expected_fallback:
                        raise ValueError("spatial diagnostic differs from its independently specified construction")
        spatial_pair = {"row_multiset_identical": True, "accepted_counts_identical": True,
                        "uncertain_rows": unknown, "uncertain_fraction": unknown / scattered["rows"],
                        "scattered_frontier12_ms": scattered["frontier12_ms"],
                        "clustered_frontier12_ms": clustered["frontier12_ms"],
                        "scattered_over_clustered_frontier12": scattered["frontier12_ms"] / clustered["frontier12_ms"],
                        "scattered_prefix": scattered["prefix"], "clustered_prefix": clustered["prefix"]}
    summary = {"schema_version": 4, "matrix_sha256": digest(matrix / "suite.json"),
               "binaries": suite["binaries"], "configuration": suite["configuration"],
               "counts": {"cases": len(rows), "ordinary_nonconstant_cases": len(ordinary),
                          "prefix_stress_cases": len(stress), "constant_controls": len(rows) - len(nonconstant),
                          "spatial_stress_cases": len(spatial),
                          "native_fresh_mask_checks": sum(row["fresh_mask_checks"] for row in rows),
                          "native_range_mask_checks": sum(row["range_mask_checks"] for row in rows),
                          "core_fresh_mask_checks": sum(row.get("core", {}).get("fresh_mask_checks", 0) for row in rows),
                          "native_complete_timing_cycles": sum(row["timing_order_audit"]["complete_cycles"] for row in rows),
                          "core_complete_timing_cycles": sum(row.get("core", {}).get("timing_order_audit", {}).get("complete_cycles", 0) for row in rows),
                          "core_partial_cycle_rounds": sum(row.get("core", {}).get("timing_order_audit", {}).get("partial_cycle_rounds", 0) for row in rows),
                          "frontier12_beats_frozen_ordinary": sum(row["frozen_over_frontier12"] > 1 for row in ordinary),
                          "frontier12_beats_best_raw_ordinary": sum(row["best_raw_over_frontier12"] > 1 for row in ordinary)},
               "ranges": {"ordinary_frozen_over_pressure": extent(row["frozen_over_pressure"] for row in ordinary),
                          "ordinary_frozen_over_frontier12": extent(row["frozen_over_frontier12"] for row in ordinary),
                          "ordinary_best_raw_over_frontier12": extent(row["best_raw_over_frontier12"] for row in ordinary),
                          "stress_frozen_over_frontier12": extent(row["frozen_over_frontier12"] for row in stress),
                          "soa_transpose_ms": extent(row["soa_transpose_ms"] for row in rows),
                          "wrapper_compilation_ms": extent(value for row in rows for value in row["wrapper_compilation_ms"].values())},
               "spatial_pair": spatial_pair, "cases": rows}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "SUMMARY.json", summary)
    lines = ["# Suffice 0.4 — measured comparisons", "",
             "All speedup ratios below divide medians from the same comparison process. Times are milliseconds. "
             "A ratio greater than one favors the denominator. Compilation, validation, copying and transposition are outside warm execution.", "",
             "The raw set includes fused interleaved (AoS) and planar (SoA) scalar/AVX2 controls plus dense/raw-vector core execution. "
             "The fixed frontier depth12 is a named heuristic; depth8/16 results remain visible and no post-hoc depth winner is treated as a deployed policy.", "",
             "## Frozen and current encoded execution", "",
             "| Case | Frozen 0.3 eager ms | Current pressure eager ms | Frontier12 ms | Frozen / pressure | Frozen / frontier12 | Best raw / frontier12 |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        name = row["case"] + (" (constant control)" if row["constant_control"] else "")
        lines.append(f"| {name} | {row['frozen_eager_ms']:.6f} | {row['pressure_eager_ms']:.6f} | {row['frontier12_ms']:.6f} | {row['frozen_over_pressure']:.3f}x | {row['frozen_over_frontier12']:.3f}x | {row['best_raw_over_frontier12']:.3f}x |")
    lines += ["", "## Raw layout controls", "",
              "| Case | Best AoS backend | AoS ms | Best SoA backend | SoA ms | AoS / SoA | SoA transpose ms |",
              "| --- | --- | ---: | --- | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['case']} | {row['best_aos_backend']} | {row['best_aos_ms']:.6f} | {row['best_soa_backend']} | {row['best_soa_ms']:.6f} | {row['aos_over_soa']:.3f}x | {row['soa_transpose_ms']:.3f} |")
    lines += ["", "## Fixed prefix depths and diagnostic fallback fractions", "",
              "Fallback fractions are computed outside timers from the exact prefix bounds of eligible 256-row batches. "
              "They describe the branch condition, not measured memory traffic. Any ambiguous bit triggers exact evaluation of all 256 rows in that batch; "
              "ambiguous logical rows and full-batch refinement rows are separate fields. Scalar/tail rows are counted separately in JSON.", "",
              "| Case | Depth8 ms | Depth12 ms | Depth16 ms | Depth8 fallback | Depth12 fallback | Depth16 fallback |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        p = row["prefix"]
        values = [f"{p[str(depth)]['fallback_batch_fraction']:.3%}" if p[str(depth)]["fallback_batch_fraction"] is not None else "—" for depth in (8, 12, 16)]
        lines.append(f"| {row['case']} | {p['8']['median_ms']:.6f} | {p['12']['median_ms']:.6f} | {p['16']['median_ms']:.6f} | " + " | ".join(values) + " |")
    if spatial_pair:
        lines += ["", "## Spatial uncertainty pair", "",
                  "These two inputs contain the same row multiset, channel ranges, and accepted count. One ambiguous row per 256 is either scattered "
                  "one per vector batch or clustered at the beginning. Only placement changes. The positive input range permits the same absolute-value folding in both.", "",
                  f"The ambiguous-row fraction is {spatial_pair['uncertain_fraction']:.6%}. The observed depth 12 scattered/clustered execution ratio is "
                  f"{spatial_pair['scattered_over_clustered_frontier12']:.3f}x. This timing ratio is measured; it is not inferred from fallback counts.", ""]
    if any("core" in row for row in rows):
        lines += ["", "## Sampled core selection", "",
                  "Generated AOT kernels are outside this selector. Selected-fixed regret and actual-instance regret are separate observations.", "",
                  "| Case | Selected | Prepare ms | Validation ms | Selected-fixed ratio | Actual-instance ratio | Work rows / budget |",
                  "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
        for row in rows:
            if "core" in row:
                c = row["core"]
                lines.append(f"| {row['case']} | {c['selected']} | {c['preparation_ms']:.3f} | {c['validation_ms']:.3f} | {c['selected_fixed_regret']:.3f}x | {c['actual_instance_regret']:.3f}x | {c['work_rows']:,} / {c['work_budget']:,} |")
    lines += ["", "## Audit scope", "",
              f"{summary['counts']['native_fresh_mask_checks']:,} fresh native complete-mask checks; "
              f"{summary['counts']['native_range_mask_checks']:,} native range checks; "
              f"{summary['counts']['core_fresh_mask_checks']:,} fresh core complete-mask checks.", "",
              "Every retained median was recomputed; generated header hashes, input identities, prefix row/batch accounting, "
              "and complete Williams-cycle position/predecessor counts were checked. This is an artifact audit, not an independent execution of every kernel.", "",
              "Constant-output controls are excluded from meaningful gain aggregates. Prefix-stress inputs are separately grouped "
              "and remain visible even when frontier execution loses. Warm synthetic measurements do not establish cold, application-wide, "
              "Windows, external-engine, or hardware-optimal performance.", ""]
    (output / "TABLES.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary["counts"], indent=2))


if __name__ == "__main__":
    main()
