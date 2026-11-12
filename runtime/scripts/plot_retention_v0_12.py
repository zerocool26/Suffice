#!/usr/bin/env python3
"""Render descriptive paired timing and retained-worker results; no inference."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.summary.read_text(encoding="utf-8"))
    if data["status"] != "passed" or data["repeats"] != 8:
        raise ValueError("This figure is designed for the completed eight-block experiment")
    args.output.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
                         "axes.spines.top": False, "axes.spines.right": False,
                         "axes.spines.left": False, "axes.edgecolor": "#d4dce4",
                         "text.color": "#192b3d", "axes.labelcolor": "#33485c",
                         "xtick.color": "#53677b", "ytick.color": "#33485c",
                         "svg.fonttype": "none"})
    fig = plt.figure(figsize=(12.2, 10.1), facecolor="white")
    grid = fig.add_gridspec(2, 1, height_ratios=[1.3, 1], left=.21, right=.96,
                           top=.80, bottom=.16, hspace=.56)
    timing, memory = fig.add_subplot(grid[0]), fig.add_subplot(grid[1])
    blue, teal, gray = "#285e9d", "#128775", "#8a9bac"
    names = [("reuse", "Reuse"), ("pressure", "Scan pressure"),
             ("mixed_queries", "Mixed queries")]
    labels = []
    for i, (scenario, label) in enumerate(names):
        for j, (budget, color) in enumerate((("large", blue), ("tight", teal))):
            y = 5 - (2 * i + j)
            v = data["scenarios"][scenario]["paired_comparisons"]["recompute_over_capture_" + budget]
            xs = v["values"]
            timing.hlines(y, min(xs), max(xs), color=color, alpha=.38, lw=1.5)
            timing.scatter(xs, [y + (k - 3.5) * .025 for k in range(8)],
                           color=color, s=24, alpha=.8, zorder=3)
            timing.scatter([v["geometric_mean"]], [y], marker="D", s=48,
                           color="#152c42", edgecolor="white", linewidth=.7, zorder=4)
            timing.text(1.705, y,
                        f'{v["geometric_mean"]:.3f}x   {v["pairs_above_one"]}/8',
                        ha="left", va="center", fontsize=9.5)
            labels.append((y, label + ("  |  16 MiB" if budget == "large" else "  |  256 KiB")))
    timing.axvline(1, color="#77889a", lw=1, ls=(0, (4, 4)))
    timing.set_yticks([y for y, _ in labels], [label for _, label in labels])
    timing.tick_params(axis="y", length=0, pad=12)
    timing.set_ylim(-.6, 5.7)
    timing.set_xlim(.78, 1.90)
    timing.set_xticks([.8, 1, 1.2, 1.4, 1.6])
    timing.set_xlabel("Paired Recompute / Capture service time  ·  above 1 favors Capture", labelpad=10)
    timing.text(1.705, 5.72, "GM       wins", va="bottom", fontsize=9, color="#53677b")
    timing.set_title("A  Capture benefit varies across workloads and repetitions", loc="left", pad=25,
                     fontsize=13, fontweight="bold")
    timing.legend(handles=[Line2D([0], [0], marker="o", linestyle="none", color=blue,
                                  label="One matched block", markersize=5),
                           Line2D([0], [0], marker="D", linestyle="none", color="#152c42",
                                  label="Paired geometric mean", markersize=5)],
                  loc="lower left", bbox_to_anchor=(-.01, 1.01), frameon=False, ncol=2, fontsize=9)
    rows = names + [("worker_churn", "Bypass-only churn")]
    for i, (scenario, label) in enumerate(rows):
        y = 3 - i
        variants = data["scenarios"][scenario]["variants"]
        strategy = "bypass" if scenario == "worker_churn" else "capture"
        large = variants[strategy + "_large"]["peak_retained_worker_bytes"]["maximum"]
        tight = variants[strategy + "_tight"]["peak_retained_worker_bytes"]["maximum"]
        memory.barh(y + .14, large / (1024 ** 2), height=.23, color=gray)
        memory.barh(y - .14, tight / (1024 ** 2), height=.23, color=teal)
        memory.text(large / (1024 ** 2) + .06, y + .14,
                    f'{large / (1024 ** 2):.3f}', va="center", fontsize=9)
        memory.text(tight / (1024 ** 2) + .06, y - .14,
                    f'{tight / (1024 ** 2):.3f}  ({100 * (1-tight/large):.2f}% lower)',
                    va="center", fontsize=9, color="#0e7163")
    memory.set_yticks([3, 2, 1, 0], [label for _, label in rows])
    memory.tick_params(axis="y", length=0, pad=12)
    memory.set_xlim(0, 4.9)
    memory.set_ylim(-.6, 3.6)
    memory.set_xlabel("Observed retained-worker peak (MiB)  ·  component accounting, not RSS", labelpad=9)
    memory.set_title("B  Independent worker retention sharply reduces the named memory pool", loc="left",
                     pad=25, fontsize=13, fontweight="bold")
    memory.legend(handles=[Patch(color=gray, label="16 MiB budget"),
                           Patch(color=teal, label="256 KiB budget")],
                  loc="lower left", bbox_to_anchor=(-.01, 1.01), frameon=False, ncol=2, fontsize=9)
    fig.text(.055, .967, "SUFFICE 0.12  /  COMPLETE-SERVICE EXPERIMENT", fontsize=11,
             fontweight="bold", color="#285e9d")
    fig.text(.055, .928, "Less retained state; measured construction savings", fontsize=23,
             fontweight="bold")
    fig.text(.055, .9, "112 fresh processes  ·  8 matched blocks per scenario  ·  5,440 complete-output checks",
             fontsize=11, color="#53677b")
    churn = data["scenarios"]["worker_churn"]["paired_comparisons"]["large_over_tight_bypass"]
    fig.text(.055, .09,
             f'Tradeoff: bypass-only churn rebuilt 48 vs 12 workers; the tight budget had '
             f'{100 * (1/churn["geometric_mean"] - 1):.2f}% longer paired-geometric service time.',
             fontsize=10.5, fontweight="bold")
    fig.text(.055, .055,
             "Timing dots and ranges are observations, not confidence intervals. Service subtracts explicitly timed oracle/audit work from raw wall time.\n"
             "One 103-file corpus split into 12 projects; one Linux configuration on a shared pinned CPU. Full raw-wall results and limitations are in the report.",
             fontsize=8.7, color="#53677b", linespacing=1.55)
    for extension in ("png", "svg"):
        fig.savefig(args.output / ("Suffice_0_12_Results." + extension), dpi=180, facecolor="white")
    plt.close(fig)


if __name__ == "__main__":
    main()
