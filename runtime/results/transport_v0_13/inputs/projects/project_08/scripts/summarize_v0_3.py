#!/usr/bin/env python3
"""Derive auditable tables from one completed 0.3 experiment matrix."""

import argparse
from collections import Counter
import json
from pathlib import Path
import statistics


RAW = {"generated-scalar", "generated-simd", "dense", "raw-vector", "dense-predicate"}
ENCODED = {"generated-encoded", "generated-encoded-scalar", "encoded-wordcode", "encoded-predicate"}


def ratio(numerator, denominator):
    return numerator / denominator if denominator and denominator > 0 else None


def format_number(value, digits=3):
    return "—" if value is None else f"{value:.{digits}f}"


def extent(values):
    values = [v for v in values if v is not None]
    return {"min": min(values), "median": statistics.median(values), "max": max(values)} if values else None


def validate_balanced_orders(orders, cycle_rows):
    """Check the recorded orders, independent of the C++ construction."""
    if not orders or not cycle_rows:
        raise ValueError("missing actual timing order")
    labels = sorted(orders[0])
    if len(set(labels)) != len(labels) or any(sorted(row) != labels for row in orders):
        raise ValueError("a timing round is not a candidate permutation")
    count = len(labels)
    if cycle_rows != (count if count == 1 or count % 2 == 0 else 2 * count):
        raise ValueError("invalid Williams cycle size")
    expected = cycle_rows // count
    for start in range(0, len(orders) - cycle_rows + 1, cycle_rows):
        rows = orders[start:start + cycle_rows]
        positions = Counter((name, column) for row in rows for column, name in enumerate(row))
        predecessors = Counter(pair for row in rows for pair in zip(row, row[1:]))
        if any(positions[name, column] != expected for name in labels for column in range(count)):
            raise ValueError("recorded timing positions are not balanced")
        if any(predecessors[a, b] != (0 if a == b else expected) for a in labels for b in labels):
            raise ValueError("recorded within-round predecessor pairs are not balanced")
    return {"complete_cycles": len(orders) // cycle_rows,
            "partial_rounds": len(orders) % cycle_rows, "candidates": count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    suite = json.loads((args.matrix / "suite.json").read_text(encoding="utf-8"))
    if suite.get("status") != "completed":
        parser.error("the matrix must have completed successfully")
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    cases = []
    checks = 0
    native_checks = 0
    for source in suite["cases"]:
        if source["status"] != "completed":
            raise ValueError("an incomplete case cannot be summarized")
        sampled, full = source["sampled"], source["full"]
        for result in (sampled, full):
            if not result["verification"]["exact_match"]:
                raise ValueError("a case failed exactness")
            checks += result["verification"]["heldout_mask_checks"]
            order = result.get("fresh_execution", {})
            if "cycle_rows" in order:
                validate_balanced_orders(order["round_order"], order["cycle_rows"])
        ss, fs = sampled["selection"], full["selection"]
        if ss["total_calibration_row_evaluations"] > ss["calibration_row_budget"]:
            raise ValueError("sampled row budget exceeded")
        if sampled["verification"]["mask_checksum"] != full["verification"]["mask_checksum"]:
            raise ValueError("paired mode masks disagree")
        entry = {
            "case": source["name"], "family": source["family"], "pattern": source["pattern"],
            "rows": source["rows"], "channels": source["channels"], "block_rows": source["block_rows"],
            "packed_only_selection": source["packed_only_selection"],
            "sampled_backend": ss["selected"], "full_backend": fs["selected"],
            "sampled_prepare_ms": ss["total_prepare_ms"], "full_prepare_ms": fs["total_prepare_ms"],
            "prepare_speedup": ratio(fs["total_prepare_ms"], ss["total_prepare_ms"]),
            "sampled_validation_ms": sampled["preparation"]["validation_ms"],
            "full_validation_ms": full["preparation"]["validation_ms"],
            "validated_prepare_speedup": ratio(full["preparation"]["validation_ms"] + fs["total_prepare_ms"],
                                                 sampled["preparation"]["validation_ms"] + ss["total_prepare_ms"]),
            "sampled_rows": ss["calibrated_rows"], "sampled_work_rows": ss["total_calibration_row_evaluations"],
            "full_work_rows": fs["total_calibration_row_evaluations"], "row_budget": ss["calibration_row_budget"],
            "work_reduction": ratio(fs["total_calibration_row_evaluations"], ss["total_calibration_row_evaluations"]),
            "sampled_rounds": ss["rounds"], "soft_deadline_reached": ss["time_budget_reached"],
            "sampled_workspace_bytes": ss["calibration_workspace_bytes"],
            "full_workspace_bytes": fs["calibration_workspace_bytes"],
            "selected_scratch_bytes": ss["selected_scratch_bytes"],
            "sampled_adaptive_median_ms": sampled["adaptive"]["timing"]["median_ms"],
            "full_adaptive_median_ms": full["adaptive"]["timing"]["median_ms"],
            "sampled_selection_regret": sampled["selection_regret"]["selected_fixed_over_best_eligible_fixed"],
            "full_selection_regret": full["selection_regret"]["selected_fixed_over_best_eligible_fixed"],
            "sampled_actual_regret": sampled["selection_regret"]["adaptive_over_best_eligible_fixed"],
            "full_actual_regret": full["selection_regret"]["adaptive_over_best_eligible_fixed"],
            "sampled_compare_peak_rss_kib": (source["sampled_execution"].get("process_usage") or {}).get("peak_rss_kib"),
            "full_compare_peak_rss_kib": (source["full_execution"].get("process_usage") or {}).get("peak_rss_kib"),
            "accepted": sampled["verification"]["accepted"],
            "mask_checksum": sampled["verification"]["mask_checksum"],
        }
        native = source.get("native_result")
        if native:
            measurement = native["measurements"]
            if not measurement["exact"] or measurement["mask_checksum"] != entry["mask_checksum"]:
                raise ValueError("native comparison failed or mask differs from paired core run")
            native_checks += measurement["fresh_mask_checks"]
            if "timing_cycle_rows" in measurement:
                validate_balanced_orders(measurement["timing_order"], measurement["timing_cycle_rows"])
            candidates = {c["backend"]: c for c in measurement["candidates"]}
            best_raw = min((c for n, c in candidates.items() if n in RAW), key=lambda c: c["median_ms"])
            encoded = [c for n, c in candidates.items() if n in ENCODED]
            best_encoded = min(encoded, key=lambda c: c["median_ms"]) if encoded else None
            generated = candidates.get("generated-encoded")
            generated_scalar = candidates.get("generated-encoded-scalar")
            recursive = candidates.get("encoded-predicate")
            wordcode = candidates.get("encoded-wordcode")
            entry["native"] = {
                "best_raw_backend": best_raw["backend"], "best_raw_ms": best_raw["median_ms"],
                "generated_raw_scalar_ms": candidates["generated-scalar"]["median_ms"],
                "generated_raw_simd_ms": candidates["generated-simd"]["median_ms"],
                "generated_encoded_ms": generated["median_ms"] if generated else None,
                "generated_encoded_scalar_ms": generated_scalar["median_ms"] if generated_scalar else None,
                "generated_encoded_scalar_over_dispatched": ratio(generated_scalar["median_ms"], generated["median_ms"]) if generated_scalar and generated else None,
                "best_encoded_backend": best_encoded["backend"] if best_encoded else None,
                "best_encoded_ms": best_encoded["median_ms"] if best_encoded else None,
                "best_encoded_speedup_over_best_raw": ratio(best_raw["median_ms"], best_encoded["median_ms"]) if best_encoded else None,
                "generated_encoded_speedup_over_best_raw": ratio(best_raw["median_ms"], generated["median_ms"]) if generated else None,
                "recursive_over_wordcode": ratio(recursive["median_ms"], wordcode["median_ms"]) if recursive and wordcode else None,
                "wordcode_over_generated_encoded": ratio(wordcode["median_ms"], generated["median_ms"]) if wordcode and generated else None,
                "kernel_compile_ms": native["kernel_compilation"]["wall_ms"],
                "emission_process_ms": native["emission_process_wall_ms"],
                "native_copy_ms": measurement["native_copy_ms"],
                "native_copy_bytes": measurement["native_copy_bytes"],
                "generated_prepare_ms": measurement["generated_prepare_ms"],
                "encoded_plane_count": measurement["encoded_plane_count"],
                "wordcode_nodes": measurement["wordcode_nodes"],
                "all_candidates": measurement["candidates"],
            }
        cases.append(entry)
    summary = {
        "schema_version": 3, "matrix": str(args.matrix), "binary": suite["binary"],
        "environment": suite["environment"], "configuration": suite["configuration"],
        "counts": {"cases": len(cases), "paired_core_comparisons": len(cases) * 2,
                   "native_comparisons": sum("native" in c for c in cases),
                   "core_fresh_full_mask_checks": checks, "native_fresh_full_mask_checks": native_checks,
                   "total_fresh_full_mask_checks": checks + native_checks,
                   "sampled_deadline_flags": sum(c["soft_deadline_reached"] for c in cases),
                   "sampled_early_stops": sum(c["soft_deadline_reached"] and c["sampled_rounds"] < suite["configuration"]["calibration_repeat"] for c in cases)},
        "ranges": {key: extent(c[key] for c in cases) for key in (
            "sampled_prepare_ms", "full_prepare_ms", "prepare_speedup", "validated_prepare_speedup",
            "sampled_selection_regret", "full_selection_regret", "sampled_actual_regret",
            "sampled_work_rows", "work_reduction", "sampled_workspace_bytes", "full_workspace_bytes",
            "sampled_compare_peak_rss_kib", "full_compare_peak_rss_kib")},
        "cases": cases,
        "scope": [
            "Ratios are paired within a case and the named process; different harness measurements are never divided.",
            "Preparation ratios omit separately completed validation unless named validated_prepare_speedup.",
            "Selection regret compares the selected fixed backend with the fastest eligible fixed backend on fresh full runs.",
            "AOT kernels are stronger diagnostic controls and are not eligible in the automatic selector.",
            "Native copies and compilation are charged separately; warm execution ratios exclude both.",
            "Comparison-process RSS includes all fixed runners and mappings, not only the selected backend.",
            "No confidence interval, external-system superiority, or end-to-end application gain is inferred.",
        ],
    }
    (output / "SUMMARY.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    lines = ["# Suffice 0.3 measured tables", "", "Derived from one completed matrix by `scripts/summarize_v0_3.py`.",
             "All ratios use medians from the same named comparison process. Execution times are milliseconds.", "",
             "## Paired preparation and selection", "",
             "Preparation excludes the separately reported full-data validation. Regret uses the selected fixed backend's fresh median divided by the best eligible fixed median; 1 is best.", "",
             "| Case | Sampled prepare ms | Full prepare ms | Reduction | Sampled choice | Sampled regret | Full regret | Work rows |",
             "| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |"]
    for c in cases:
        lines.append(f"| {c['case']} | {c['sampled_prepare_ms']:.3f} | {c['full_prepare_ms']:.3f} | {c['prepare_speedup']:.2f}x | {c['sampled_backend']} | {c['sampled_selection_regret']:.3f}x | {c['full_selection_regret']:.3f}x | {c['sampled_work_rows']:,} |")
    lines += ["", "## Fused native and encoded comparisons", "",
              "Best raw includes generated scalar/AVX2, dense, raw-vector and dense-predicate. Raw/encoded speedup greater than 1 favors encoded execution. A dash means no supported encoded circuit. Native compilation, validation and copying are outside execution timers.", "",
              "| Case | Best raw | Raw ms | Fused encoded ms | Raw / fused encoded | Best encoded | Best encoded ms | Raw / best encoded |",
              "| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |"]
    for c in cases:
        if "native" not in c:
            continue
        n = c["native"]
        lines.append(f"| {c['case']} | {n['best_raw_backend']} | {n['best_raw_ms']:.4f} | {format_number(n['generated_encoded_ms'],4)} | {format_number(n['generated_encoded_speedup_over_best_raw'],2)} | {n['best_encoded_backend'] or '—'} | {format_number(n['best_encoded_ms'],4)} | {format_number(n['best_encoded_speedup_over_best_raw'],2)} |")
    lines += ["", "## Setup costs and memory scope", "",
              "Calibration workspace is the documented work-buffer model. RSS is the peak of the entire compare process, including all fixed controls, mapped data and output masks. Native driver compilation includes generated kernels and the benchmark driver; the cached core support library is recorded separately in each native result.", "",
              "| Case | Validation ms (sampled run) | Sampled workspace KiB | Full workspace KiB | Compare RSS MiB | Native compile ms | Native copy ms |",
              "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for c in cases:
        n = c.get("native", {})
        rss = c["sampled_compare_peak_rss_kib"]
        lines.append(f"| {c['case']} | {c['sampled_validation_ms']:.3f} | {c['sampled_workspace_bytes']/1024:.2f} | {c['full_workspace_bytes']/1024:.2f} | {format_number(rss/1024 if rss is not None else None,2)} | {format_number(n.get('kernel_compile_ms'),1)} | {format_number(n.get('native_copy_ms'),3)} |")
    lines += ["", "## Verification totals", "", "```json", json.dumps(summary["counts"], indent=2), "```", ""]
    (output / "TABLES.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary["counts"], indent=2))


if __name__ == "__main__":
    main()
