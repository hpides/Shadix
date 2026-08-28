#!/usr/bin/env python3
"""Plot per-system latency CDFs as stacked subplots, one row per system.

A row is a single panel, or — when the system was also measured in a
second mode given with ``--async-system`` — a left/right pair of panels
(sync on the left, async on the right by default).
"""

import argparse
import sys
import re
from pathlib import Path

import numpy as np
import polars as pl
import seaborn as sns
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

# plot_layout.py lives next to this script; make the import independent of
# how the script is invoked (by path from any cwd, or as a module).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_layout import make_row_axes, pair_rows, positive_float  # noqa: E402

PALETTE = [
    "#f5a700",
    "#dc6007",
    "#b00539",
    "#6b009c",
    "#006d5b",
    "#0073e6",
    "#e6007a",
    "#00C800",
    "#FFD500",
    "#0033A0",
]


# Matches the size field of every rtt result filename convention:
# rtt_s8_... (no transport tag), rtt_<transport>_s8_... (e.g.
# rtt_list_s8_..., rtt_core_s8_..., rtt_non-persistent_s8_...), plus the
# `rtt --send-mode async` variants that append -async to the transport
# (rtt_list-async_s8_..., rtt_core-async_s8_..., ...).
# The optional transport tag is letters/hyphens only, so it can never
# swallow the s<digits> size field or match digits elsewhere.
SIZE_RE = re.compile(r"rtt_(?:[a-z-]+_)?s(\d+)_")


def _parse_system_specs(parser, flag: str, specs):
    systems = []
    for spec in specs or []:
        if len(spec) < 2:
            parser.error(f"{flag} needs a label and at least one CSV file, got: {spec}")
        systems.append((spec[0], [Path(p) for p in spec[1:]]))
    return systems


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Plot latency CDFs for any number of systems as a stacked subplot "
            "figure; --async-system adds a right-hand panel to a system's row."
        )
    )
    parser.add_argument(
        "--system",
        action="append",
        nargs="+",
        metavar=("LABEL", "FILE"),
        dest="systems",
        required=True,
        help=(
            "System to plot: a display label followed by its RTT CSV files. "
            "Repeat the flag once per system; rows appear in the given order."
        ),
    )
    parser.add_argument(
        "--async-system",
        action="append",
        nargs="+",
        metavar=("LABEL", "FILE"),
        dest="async_systems",
        help=(
            "Right-hand panel for the --system row with the same LABEL (e.g. "
            "the `rtt --send-mode async` files). Rows without one span both "
            "columns."
        ),
    )
    parser.add_argument(
        "--column-titles",
        nargs=2,
        metavar=("LEFT", "RIGHT"),
        default=("sync", "async"),
        help="Suffixes for the panel titles of paired rows (default: sync async)",
    )
    parser.add_argument(
        "--width",
        type=positive_float,
        default=None,
        help="Figure width in inches (default: 4, i.e. one paper column)",
    )
    parser.add_argument(
        "--discard-start",
        type=float,
        default=1.0,
        help="Seconds to discard from start of measurement (default: 1.0)",
    )
    parser.add_argument(
        "--discard-end",
        type=float,
        default=1.0,
        help="Seconds to discard from end of measurement (default: 1.0)",
    )
    parser.add_argument(
        "--xlim",
        type=float,
        nargs=2,
        metavar=("MIN", "MAX"),
        help="X-axis limits (µs) applied to every subplot",
    )
    parser.add_argument(
        "--percentile",
        type=float,
        default=None,
        help="Auto-set each subplot's x-axis max to this percentile of its own data",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output base name without extension (saves .svg and .pdf)",
    )
    args = parser.parse_args()

    args.systems = _parse_system_specs(parser, "--system", args.systems)
    args.async_systems = _parse_system_specs(parser, "--async-system", args.async_systems)
    args.rows = pair_rows(args.systems, args.async_systems, parser.error)
    return args


def extract_size(filename: str) -> int:
    m = SIZE_RE.search(filename)
    if not m:
        raise ValueError(f"Could not extract size from {filename}")
    return int(m.group(1))


def load_and_filter(
    file: Path, discard_start: float, discard_end: float
) -> pl.DataFrame:
    df = pl.read_csv(file)
    if len(df) == 0:
        # A receiver that got zero messages still writes the CSV header;
        # return an empty typed frame so the caller's "no RTT samples"
        # warning path handles it instead of a TypeError on min().
        return pl.DataFrame(
            schema={
                "send_time_ns": pl.Int64,
                "receive_time_ns": pl.Int64,
                "rtt_us": pl.Float64,
            }
        )
    min_time = df["send_time_ns"].min()
    max_time = df["send_time_ns"].max()
    start_cutoff = min_time + int(discard_start * 1e9)
    end_cutoff = max_time - int(discard_end * 1e9)
    df = df.filter(
        (pl.col("send_time_ns") >= start_cutoff)
        & (pl.col("send_time_ns") <= end_cutoff)
    )
    df = df.with_columns(
        ((pl.col("receive_time_ns") - pl.col("send_time_ns")) / 1000).alias("rtt_us")
    )
    return df


def compute_cdf(values: pl.Series) -> tuple[list[float], list[float]]:
    sorted_vals = sorted(values.to_list())
    n = len(sorted_vals)
    cdf = [(i + 1) / n for i in range(n)]
    return sorted_vals, cdf


def print_latency_stats(system: str, size: int, values: list[float]) -> None:
    system_label = system.lower()
    if not values:
        print(f"[{system_label}] size {size}B: Warning: no RTT samples after filtering")
        return

    print(
        f"[{system_label}] size {size}B: "
        f"min: {min(values):.2f} µs, "
        f"median: {float(np.median(values)):.2f} µs, "
        f"p99: {float(np.percentile(values, 99)):.2f} µs"
    )


def plot_system(
    ax,
    files: list[Path],
    discard_start: float,
    discard_end: float,
    xlim,
    percentile,
    title: str,
    color_for_size: dict[int, tuple],
    stats_label: str | None = None,
) -> None:
    plot_data = []
    all_rtt = []
    for f in files:
        df = load_and_filter(f, discard_start, discard_end)
        size = extract_size(f.name)
        x, y = compute_cdf(df["rtt_us"])
        all_rtt.extend(x)
        plot_data.append((size, f"{size} B", x, y))

    plot_data.sort(key=lambda item: item[0])
    for size, label, x, y in plot_data:
        ax.plot(x, y, label=label, color=color_for_size[size])
        print_latency_stats(stats_label or title, size, x)

    ax.set_title(title)
    ax.set_ylim(0, 1)
    ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=5))
    ax.xaxis.set_major_formatter(FuncFormatter(format_us))

    if xlim is not None:
        ax.set_xlim(xlim[0], xlim[1])
    elif percentile is not None and all_rtt:
        ax.set_xlim(0, float(np.percentile(all_rtt, percentile)))


def format_us(value, _pos):
    """x-tick formatter: 8000 -> "8k"."""
    if abs(value) >= 1000:
        return f"{value / 1000:g}k"
    return f"{value:g}"


def main():
    args = parse_args()
    rows = args.rows
    two_columns = any(async_files is not None for _, _, async_files in rows)
    n_rows = len(rows)

    sns.set_theme(style="whitegrid")
    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"] = 42
    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.serif"] = ["Linux Libertine", "Linux Libertine O", "Libertinus Serif"]
    width = args.width if args.width is not None else 4.0
    fig = plt.figure(figsize=(width, 1.6 * n_rows), constrained_layout=True)
    row_axes, legend_ax = make_row_axes(
        fig,
        [async_files is not None for _, _, async_files in rows],
        legend_slot=two_columns,
    )

    all_sizes = sorted(
        {
            extract_size(f.name)
            for _, files, async_files in rows
            for f in files + (async_files or [])
        }
    )
    if len(all_sizes) > len(PALETTE):
        raise ValueError(
            f"color palette only has {len(PALETTE)} colors; got {len(all_sizes)} sizes"
        )
    color_for_size = dict(zip(all_sizes, PALETTE))

    left_title, right_title = args.column_titles
    for axes, (label, files, async_files) in zip(row_axes, rows):
        panels = [(axes[0], files, label, label)]
        if async_files is not None:
            panels = [
                (axes[0], files, f"{label} ({left_title})", label),
                (axes[1], async_files, f"{label} ({right_title})", f"{label} {right_title}"),
            ]
        for ax, panel_files, title, stats_label in panels:
            plot_system(
                ax,
                panel_files,
                args.discard_start,
                args.discard_end,
                args.xlim,
                args.percentile,
                title,
                color_for_size,
                stats_label=stats_label,
            )
        if async_files is not None:
            # Half-width panels: the CDF scale is the same on both sides, so
            # the right panel drops its y labels.
            for ax in axes:
                ax.xaxis.set_major_locator(plt.MaxNLocator(nbins=5))
            axes[1].tick_params(labelleft=False)
        else:
            # Integer tick steps (no 2.5-unit steps on single-digit ranges).
            axes[0].xaxis.set_major_locator(
                plt.MaxNLocator(nbins=5, steps=[1, 2, 5, 10])
            )

    if two_columns:
        fig.supxlabel("RTT (µs)")
    else:
        row_axes[-1][0].set_xlabel("RTT (µs)")
    fig.supylabel("CDF")

    handles = [
        plt.Line2D([0], [0], color=color_for_size[s], label=f"{s} B") for s in all_sizes
    ]
    if legend_ax is not None:
        # Two-column layout: the legend lives in the slot carved out of the
        # spanning row instead of adding width to the figure, flush with the
        # right edge of the panels below it. It is excluded from the layout
        # computation: otherwise constrained_layout books its overhang as a
        # margin of the slot's grid column and that margin widens the gap
        # between every sync | async pair.
        legend = legend_ax.legend(
            handles=handles,
            title="Message\nsize",
            loc="center right",
            handlelength=1.5,
            borderaxespad=0,
        )
        legend.set_in_layout(False)
    else:
        legend = fig.legend(
            handles=handles,
            title="Message\nsize",
            loc="outside right center",
            handlelength=1.5,
            borderaxespad=0.3,
        )
    # The two-line title is left-aligned within itself by default.
    legend.get_title().set_multialignment("center")

    if args.output:
        svg_path = args.output.with_suffix(".svg")
        pdf_path = args.output.with_suffix(".pdf")
        fig.savefig(svg_path, bbox_inches="tight")
        fig.savefig(pdf_path, bbox_inches="tight")
        print(f"Saved plot to {svg_path} and {pdf_path}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
