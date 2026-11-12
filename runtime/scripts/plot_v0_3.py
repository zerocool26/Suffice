#!/usr/bin/env python3
"""Optional Matplotlib visualization of the measured release summary."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter

    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    cases = summary["cases"]
    encoded = [c for c in cases
               if c.get("native", {}).get("generated_encoded_speedup_over_best_raw") is not None
               and c["native"].get("encoded_plane_count", 0) > 0]
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.spines.top": False,
                         "axes.spines.right": False, "axes.spines.left": False,
                         "axes.edgecolor": "#d2d8e2", "text.color": "#162238", "axes.labelcolor": "#162238"})
    fig, axes = plt.subplots(1, 2, figsize=(17, 10.5), gridspec_kw={"wspace": 0.75})
    fig.patch.set_facecolor("#fcfcfe")
    for ax in axes:
        ax.set_facecolor("#fcfcfe")
        ax.set_xscale("log")
        ax.axvline(1, color="#6b7280", linewidth=1.0, linestyle="--", zorder=1)
        ax.grid(axis="x", color="#e2e6ed", linewidth=0.6, zorder=0)
        ax.tick_params(axis="y", length=0)
        ax.xaxis.set_major_formatter(FuncFormatter(lambda x, pos: f"{x:g}×"))

    def labels(case):
        label = case["case"].replace("_", " ")
        native = case.get("native", {})
        if (native.get("generated_encoded_speedup_over_best_raw") is not None
                and native.get("encoded_plane_count") == 0):
            label += " (constant output)"
        return label
    for y, case in enumerate(encoded):
        value = case["native"]["generated_encoded_speedup_over_best_raw"]
        color = "#087f8c" if value >= 1 else "#b54742"
        axes[0].plot([1, value], [y, y], color=color, linewidth=2.0, zorder=2)
        axes[0].scatter([value], [y], s=48, color=color, zorder=3)
        axes[0].annotate(f"{value:.2f}×", (value, y), xytext=(7, 0), textcoords="offset points", va="center", fontsize=9)
    axes[0].set_yticks(range(len(encoded)), [labels(c) for c in encoded])
    axes[0].invert_yaxis()
    axes[0].set_title("Fused encoded execution\nagainst the fastest included raw control", loc="left", fontsize=14, fontweight="bold", pad=18)
    axes[0].set_xlabel("Raw median ÷ fused encoded median; higher favors encoded", labelpad=12)
    if encoded:
        values = [c["native"]["generated_encoded_speedup_over_best_raw"] for c in encoded]
        axes[0].set_xlim(min(0.5, min(values) * 0.6), max(2, max(values) * 2.5))

    for y, case in enumerate(cases):
        warm = case["prepare_speedup"]
        validated = case["validated_prepare_speedup"]
        axes[1].plot([validated, warm], [y, y], color="#b8c8de", linewidth=1.4, zorder=1)
        axes[1].scatter(warm, y, s=43, color="#315da8", zorder=3,
                        label="Preparation after validation" if y == 0 else None)
        axes[1].scatter(validated, y, s=32, color="#e5a126", marker="D", zorder=3,
                        label="Validation + preparation" if y == 0 else None)
    axes[1].set_yticks(range(len(cases)), [labels(c) for c in cases], fontsize=9)
    axes[1].invert_yaxis()
    # Reserve space for the legend without obscuring the final measurement.
    axes[1].set_ylim(len(cases) + 1.2, -1)
    axes[1].set_title("Bounded sampled preparation\nagainst complete-data calibration", loc="left", fontsize=14, fontweight="bold", pad=18)
    axes[1].set_xlabel("Full-mode cost ÷ sampled-mode cost; higher favors sampling", labelpad=12)
    axes[1].legend(loc="lower right", fontsize=9, frameon=True, facecolor="white")
    all_preparation = [c[k] for c in cases for k in ("prepare_speedup", "validated_prepare_speedup")]
    axes[1].set_xlim(min(0.5, min(all_preparation) * 0.7), max(all_preparation) * 1.8)
    fig.suptitle("Suffice 0.3  |  Measured gains and the cost of preparing them", x=0.06, ha="left", fontsize=20, fontweight="bold")
    fig.text(0.06, 0.925,
             f"Synthetic integer workloads • Linux x86-64 • {summary['configuration']['repeat']} fresh repetitions per implementation • one pinned CPU",
             fontsize=11, color="#526177")
    fig.text(0.06, 0.035,
             "Left: same-process warm medians; raw controls include generated scalar and AVX2 kernels. Compilation, input copy and packing excluded.\n"
             "Right: paired preparation modes on the same inputs. Full validation remains necessary. No application-wide speedup or confidence interval is inferred.\n"
             "Most cases contain 1,048,576 rows; sign scale contains 16,777,216. Constant/unsupported encoded cases appear only on the right. Source: results/SUMMARY.json.",
             fontsize=10, color="#526177", linespacing=1.6)
    fig.subplots_adjust(left=0.18, right=0.97, top=0.85, bottom=0.15)
    args.output.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output / "performance.png", dpi=160, facecolor=fig.get_facecolor())
    fig.savefig(args.output / "performance.svg", facecolor=fig.get_facecolor())
    plt.close(fig)
    print("Saved performance.png and performance.svg")


if __name__ == "__main__":
    main()
