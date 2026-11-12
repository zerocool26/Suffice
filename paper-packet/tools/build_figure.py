#!/usr/bin/env python3
"""Render the paper's descriptive paired observations from frozen SUMMARY.json."""
import argparse
import hashlib
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


def build(source, directory):
    summary = json.loads(source.read_text())
    fig, ax = plt.subplots(figsize=(6.65, 2.15), dpi=360)
    order = [('reuse','Reuse'), ('pressure','Pressure'), ('mixed_queries','Mixed'),
             ('computed','Computed'), ('empty','Empty')]
    jitter = [-.12, .10, -.03, .18, -.18, .04, .13, -.08, .00, .20]
    for row, (key, label) in enumerate(order):
        data = summary['scenarios'][key]['paired_comparisons']['jsonl_over_binary_service']
        y = 4-row
        color = '#256b78' if key != 'empty' else '#995f2c'
        ax.scatter(data['values'], [y+j for j in jitter], s=14, color=color, alpha=.72,
                   edgecolors='white', linewidths=.25, zorder=3)
        ax.scatter([data['geometric_mean']], [y], marker='D', s=37, color='white',
                   edgecolors='#172d3b', linewidths=1.1, zorder=5)
        ax.plot([data['median']]*2, [y-.28,y+.28], color='#151b24', lw=1.1, zorder=4)
    ax.axvline(1, color='#66717c', ls=(0,(3,3)), lw=.8, zorder=1)
    ax.set_xlim(.8,2.75)
    ax.set_ylim(-.48,4.58)
    ax.set_yticks(range(5), [label for _,label in reversed(order)], fontsize=9)
    ax.set_xticks([1,1.5,2,2.5])
    ax.tick_params(axis='x', labelsize=8.7, color='#8a949d', length=3)
    ax.tick_params(axis='y', length=0, pad=8)
    ax.set_xlabel('JSONL / binary service time  (above 1 favors binary)', fontsize=9, labelpad=5)
    ax.set_axisbelow(True)
    ax.grid(axis='x', color='#e8ebed', lw=.6)
    for edge in ['top','right','left']:
        ax.spines[edge].set_visible(False)
    ax.spines['bottom'].set_color('#aab2ba')
    legend = [Line2D([0],[0],marker='o',color='none',markerfacecolor='#256b78',markeredgecolor='none',markersize=4,label='Paired block'),
              Line2D([0],[0],marker='D',color='none',markerfacecolor='white',markeredgecolor='#172d3b',markersize=4.7,label='Geometric mean'),
              Line2D([0],[0],marker='|',color='none',markeredgecolor='#151b24',markersize=8,label='Median')]
    ax.legend(handles=legend,loc='lower center',bbox_to_anchor=(.52,1.00),ncol=3,
              frameon=False,fontsize=8.2,handletextpad=.4,columnspacing=1.8)
    fig.subplots_adjust(left=.135,right=.985,bottom=.21,top=.85)
    directory.mkdir(parents=True, exist_ok=True)
    for ext in ['png','svg']:
        fig.savefig(directory/f'transport_paper.{ext}', dpi=360, facecolor='white')
    plt.close(fig)
    receipt = {'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
               'observations':50,'statistic':'within-repeat JSONL / binary service time',
               'uncertainty':'descriptive points only; no confidence intervals',
               'omitted_observations':0,
               'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.glob('transport_paper.*'))}}
    (directory/'FIGURE_RECEIPT.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    build(args.summary,args.output_dir)
