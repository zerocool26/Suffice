#!/usr/bin/env python3
"""Plot retained transport observations; never launches the native runtime."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, default=Path("results/transport_v0_13/SUMMARY.json"))
    parser.add_argument("--output", type=Path, default=Path("figures"))
    args = parser.parse_args()
    raw = args.summary.read_bytes()
    summary = json.loads(raw)
    if summary["status"] != "passed" or summary["repeats"] != 10:
        raise ValueError("This release figure expects the completed ten-pair experiment")
    names = ["reuse", "pressure", "mixed_queries", "computed", "empty"]
    labels = ["Repeated results", "Cache pressure", "Mixed predicates", "Computed results", "Empty output"]
    analysis = {"scope": "Derived presentation of retained observations, no new native execution",
                "summary_sha256": hashlib.sha256(raw).hexdigest(), "scenarios": {}}
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.spines.left": False, "axes.edgecolor": "#cbd5e1",
                         "axes.labelcolor": "#334155", "xtick.color": "#475569",
                         "ytick.color": "#1e293b", "savefig.facecolor": "white"})
    fig, (left, right) = plt.subplots(1, 2, figsize=(13.8, 6.8), gridspec_kw={"width_ratios": [1.38, 1]})
    for i, (name, label) in enumerate(zip(names, labels)):
        scenario = summary["scenarios"][name]
        pair = scenario["paired_comparisons"]
        service = pair["jsonl_over_binary_service"]
        wire = pair["response_byte_reduction"]
        color = "#bd641a" if name == "empty" else "#087f8c"
        jitter = np.linspace(-.13, .13, len(service["values"]))
        left.scatter(service["values"], i+jitter, s=30, color=color, alpha=.55, linewidth=0, zorder=3)
        left.scatter([service["geometric_mean"]], [i], marker="D", s=63,
                     color="#122d42", edgecolors="white", linewidths=.8, zorder=5)
        left.plot([service["median"], service["median"]], [i-.19, i+.19], color="#122d42", lw=1.8, zorder=4)
        left.text(2.73, i, f'{service["geometric_mean"]:.2f}×', va="center", ha="right", fontsize=11, weight="bold")
        percent = 100*wire["mean"]
        right.barh(i, percent, color=color, height=.49, alpha=.9)
        right.text(percent+1.0 if percent>=0 else 1.0, i, f'{percent:+.2f}%', va="center", ha="left", weight="bold", fontsize=11)
        analysis["scenarios"][name] = {
            "label": label, "pairs": len(service["values"]),
            "service_geometric_ratio_jsonl_over_binary": service["geometric_mean"],
            "service_equivalent_reduction_percent": 100*(1-1/service["geometric_mean"]),
            "service_paired_median_ratio": service["median"],
            "service_observed_ratio_range": [service["minimum"], service["maximum"]],
            "service_pairs_favoring_binary": service["pairs_above_one"],
            "service_median_ms": {k:v["service_ms"]["median"] for k,v in scenario["variants"].items()},
            "raw_wall_geometric_ratio_jsonl_over_binary": pair["jsonl_over_binary_raw_wall"]["geometric_mean"],
            "query_time_geometric_ratio_jsonl_over_binary": pair["jsonl_over_binary_query_ms"]["geometric_mean"],
            "response_byte_reduction_mean_percent": percent,
            "response_bytes_median": {k:v["wire_bytes_received"]["median"] for k,v in scenario["variants"].items()},
            "matched_final_stats": scenario["matched_final_stats"],
            "matched_request_wire_bytes": scenario["matched_request_wire_bytes"]}
    left.axvline(1, color="#94a3b8", ls="--", lw=1.3, zorder=1)
    right.axvline(0, color="#94a3b8", lw=1.2, zorder=1)
    left.set_xlim(.75, 2.82)
    right.set_xlim(-6, 61)
    for axis in (left, right):
        axis.set_ylim(4.6, -.6)
        axis.set_yticks(range(5))
        axis.tick_params(axis="y", length=0)
        axis.grid(axis="x", alpha=.16, zorder=0)
        axis.set_axisbelow(True)
    left.set_yticklabels(labels)
    right.set_yticklabels([])
    left.set_xticks([1, 1.5, 2, 2.5], ["1×", "1.5×", "2×", "2.5×"])
    left.set_xlabel("JSONL / binary complete-service time")
    right.set_xlabel("Mean reduction in complete-session response bytes")
    left.set_title("A   Complete-service ratios", loc="left", fontsize=13, weight="bold", pad=14)
    right.set_title("B   Response traffic", loc="left", fontsize=13, weight="bold", pad=14)
    fig.suptitle("Suffice 0.13 | Bounded binary responses", x=.035, ha="left", y=.965, fontsize=21, weight="bold", color="#122d42")
    fig.text(.035, .91, "Same ordered row IDs, paths and complete bodies • 100 fresh processes • 4,840 output checks", fontsize=12, color="#475569")
    fig.text(.035, .115, "Dots: 10 paired process blocks per scenario. Diamond: geometric mean. Vertical mark: paired median.", fontsize=10.3, color="#475569")
    fig.text(.035, .079, "Empty output: 4/10 timing wins; paired median 0.99×. The 1.07× mean is sensitive to an admission-time outlier.", fontsize=10.3, color="#8c4c17")
    fig.text(.035, .043, "Linux, one shared pinned CPU and one corpus. Service = raw session wall minus timed oracle audits. Descriptive results, not confidence intervals.", fontsize=9.6, color="#64748b")
    fig.subplots_adjust(left=.155, right=.97, top=.80, bottom=.235, wspace=.12)
    args.output.mkdir(parents=True, exist_ok=True)
    for extension in ("png", "svg"):
        fig.savefig(args.output / ("transport_v0_13."+extension), dpi=180)
    (args.output / "transport_v0_13_data.json").write_text(json.dumps(analysis, indent=2, sort_keys=True)+"\n")
    print(json.dumps({"status":"passed", "figure_data_source_sha256":analysis["summary_sha256"],
                      "outputs":[str(args.output/('transport_v0_13.'+s)) for s in ['png','svg']]}))


if __name__ == "__main__":
    main()
