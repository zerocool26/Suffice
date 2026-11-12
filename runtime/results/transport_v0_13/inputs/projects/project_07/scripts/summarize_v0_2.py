#!/usr/bin/env python3
"""Derive auditable tables from completed Suffice schema-2 comparison suites."""

import argparse
import collections
import csv
import json
from pathlib import Path
import statistics


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=root / "results/release_v0_2")
    parser.add_argument("--output", type=Path, default=root / "results")
    args = parser.parse_args()
    cases = []
    backends = []
    hashes = set()
    suites = sorted(args.matrix.glob("*/suite.json"))
    if not suites:
        raise SystemExit("No comparison suites found")
    for path in suites:
        suite = json.loads(path.read_text(encoding="utf-8"))
        if suite["status"] != "completed":
            raise SystemExit(f"Incomplete suite: {path}")
        hashes.add(suite["binary"]["sha256"])
        for case in suite["cases"]:
            result = case["comparison"]
            checks = result["verification"]
            if not all(checks[key] for key in (
                "exact_match", "payload_checksum_verified", "raw_pair_verified",
                "checked_every_repetition",
            )):
                raise SystemExit(f"Missing exact verification: {path}/{case['pattern']}")
            best = result["best"]
            row = {
                "suite": path.parent.name,
                "pattern": case["pattern"],
                "rows": result["dataset"]["rows"],
                "channels": result["dataset"]["channels"],
                "block_rows": result["dataset"]["block_rows"],
                "raw_available_to_selector": result["representation_policy"]["adaptive_has_raw"],
                "selected_backend": result["adaptive"]["backend"],
                "selected_kernel": result["adaptive"]["stats"]["kernel"],
                "selected_median_ms": result["adaptive"]["timing"]["median_ms"],
                "selected_min_ms": result["adaptive"]["timing"]["min_ms"],
                "selected_max_ms": result["adaptive"]["timing"]["max_ms"],
                "best_raw_backend": best["raw_fixed"]["backend"],
                "best_raw_median_ms": best["raw_fixed"]["median_ms"],
                "best_packed_backend": best["packed_fixed"]["backend"],
                "best_packed_median_ms": best["packed_fixed"]["median_ms"],
                "raw_over_packed": result["comparisons"]["best_raw_over_best_packed"],
                "best_eligible_backend": best["eligible_selection_fixed"]["backend"],
                "best_eligible_median_ms": best["eligible_selection_fixed"]["median_ms"],
                "choice_regret": result["selection_regret"]["selected_fixed_over_best_eligible_fixed"],
                "execution_regret": result["selection_regret"]["adaptive_over_best_eligible_fixed"],
                "adaptive_prepare_ms": result["selection"]["total_prepare_ms"],
                "input_validation_ms": result["preparation"]["validation_ms"],
                "pack_process_wall_ms": case["packing"]["wall_seconds"] * 1000,
                "comparison_total_prepare_ms": result["preparation"]["total_comparison_prepare_ms"],
                "selected_scratch_bytes": result["selection"]["selected_scratch_bytes"],
                "calibration_workspace_bytes": result["selection"]["calibration_workspace_bytes"],
                "selected_requested_payload_bytes": result["adaptive"]["stats"]["requested_payload_bytes"],
                "output_bytes": result["adaptive"]["stats"]["output_bytes"],
                "accepted": checks["accepted"],
                "mask_checksum": checks["mask_checksum"],
                "fresh_mask_checks": checks["heldout_mask_checks"],
                "fixed_backend_count": len(result["fixed"]),
                "binary_sha256": suite["binary"]["sha256"],
                "source_result": f"{path.parent.name}/{case['result_file']}",
            }
            if row["fresh_mask_checks"] != (len(result["fixed"]) + 1) * result["repeat"]:
                raise SystemExit("Unexpected fresh mask-check count")
            cases.append(row)
            for label, entry in [("adaptive", result["adaptive"])] + [("fixed", item) for item in result["fixed"]]:
                backends.append({
                    "suite": row["suite"], "pattern": row["pattern"],
                    "kind": label, "backend": entry["backend"],
                    "kernel": entry["stats"]["kernel"],
                    "median_ms": entry["timing"]["median_ms"],
                    "min_ms": entry["timing"]["min_ms"],
                    "max_ms": entry["timing"]["max_ms"],
                    "samples_ms": json.dumps(entry["samples_ms"], separators=(",", ":")),
                    "scratch_bytes": entry["stats"]["peak_scratch_bytes"],
                    "requested_payload_bytes": entry["stats"]["requested_payload_bytes"],
                    "output_bytes": entry["stats"]["output_bytes"],
                })
    if len(hashes) != 1:
        raise SystemExit("Mixed binary identities; do not pool these suites")
    args.output.mkdir(parents=True, exist_ok=True)
    for name, rows in [("comparison.csv", cases), ("backend_comparison.csv", backends)]:
        with (args.output / name).open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    summary = {
        "suite_count": len(suites), "case_count": len(cases),
        "binary_sha256": next(iter(hashes)),
        "fresh_mask_checks": sum(row["fresh_mask_checks"] for row in cases),
        "all_outputs_exact": True,
        "selected_backends": dict(collections.Counter(row["selected_backend"] for row in cases)),
        "choice_exact_best_count": sum(row["choice_regret"] == 1 for row in cases),
        "choice_within_5_percent_count": sum(row["choice_regret"] <= 1.05 for row in cases),
        "choice_regret_median": statistics.median(row["choice_regret"] for row in cases),
        "choice_regret_max": max(row["choice_regret"] for row in cases),
        "execution_regret_median": statistics.median(row["execution_regret"] for row in cases),
        "execution_regret_max": max(row["execution_regret"] for row in cases),
        "adaptive_prepare_ms_range": [min(row["adaptive_prepare_ms"] for row in cases), max(row["adaptive_prepare_ms"] for row in cases)],
        "selected_scratch_bytes_range": [min(row["selected_scratch_bytes"] for row in cases), max(row["selected_scratch_bytes"] for row in cases)],
        "calibration_workspace_bytes_range": [min(row["calibration_workspace_bytes"] for row in cases), max(row["calibration_workspace_bytes"] for row in cases)],
    }
    (args.output / "release_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    lines = [
        "# Complete release matrix", "",
        "All times are milliseconds; seven fresh execution samples per entry. "
        "Raw/packed is the best implemented raw fixed median divided by the best "
        "implemented packed fixed median. Values below one favor raw. Selected "
        "time is measured separately after calibration. No setup or packing "
        "cost is included in these execution-only times.", "",
        "| Suite / pattern | Selected backend | Selected ms | Best raw ms | Best packed ms | Raw / packed | Choice regret | Prepare ms |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in cases:
        lines.append(
            f"| {row['suite']} / {row['pattern']} | {row['selected_backend']} | "
            f"{row['selected_median_ms']:.6f} | {row['best_raw_median_ms']:.6f} | "
            f"{row['best_packed_median_ms']:.6f} | {row['raw_over_packed']:.3f}x | "
            f"{row['choice_regret']:.3f}x | {row['adaptive_prepare_ms']:.3f} |"
        )
    lines += ["", "Choice regret compares the independently timed fixed implementation of "
        "the selected backend with the fastest eligible fixed implementation. "
        "Prepare is the adaptive constructor's measured cost; validation was "
        "already performed and is reported separately in comparison.csv. "
        "Packed-only FIR suites withhold raw data from selection while retaining "
        "raw controls. Boundary magnitude and mixed cases are constant results "
        "under the verified data ranges; their tiny timings are not representative "
        "scan throughput.", ""]
    (args.output / "COMPARISON.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
