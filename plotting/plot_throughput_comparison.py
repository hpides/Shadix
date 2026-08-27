#!/usr/bin/env python3
"""Plot per-system throughput as stacked subplots sharing the x-axis.

A row is a single panel, or — when the system was also measured in a
second mode given with ``--async-system`` — a left/right pair of panels
(sync on the left, async on the right by default).
"""

import argparse
import sys
import glob
from pathlib import Path

import polars as pl
import seaborn as sns
import matplotlib.pyplot as plt

# plot_layout.py lives next to this script; make the import independent of
# how the script is invoked (by path from any cwd, or as a module).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_layout import hide_inner_xticklabels, make_row_axes, pair_rows, positive_float  # noqa: E402

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


def _parse_system_specs(parser, flag: str, specs):
    systems = []
    for name, pattern, batch_size in specs or []:
        try:
            batch_size = int(batch_size)
        except ValueError:
            parser.error(f"{flag} {name}: BATCH_SIZE must be an integer, got {batch_size!r}")
        if batch_size <= 0:
            parser.error(f"{flag} {name}: BATCH_SIZE must be positive, got {batch_size}")
        systems.append((name, (pattern, batch_size)))
    return systems


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Plot throughput vs message size for any number of systems as a "
            "stacked subplot figure; --async-system adds a right-hand panel to "
            "a system's row."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--system",
        action="append",
        nargs=3,
        metavar=("NAME", "PATTERN", "BATCH_SIZE"),
        dest="systems",
        required=True,
        help=(
            "System to plot: display name, file pattern with {size} and {id} "
            "placeholders, and items per batch. Repeat the flag once per "
            "system; rows appear in the given order."
        ),
    )
    parser.add_argument(
        "--async-system",
        action="append",
        nargs=3,
        metavar=("NAME", "PATTERN", "BATCH_SIZE"),
        dest="async_systems",
        help=(
            "Right-hand panel for the --system row with the same NAME (e.g. "
            "the `--send-mode async` / pipelined files). Rows without one span "
            "both columns."
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
        "--sizes", type=int, nargs="+", required=True, help="Item sizes (bytes) to plot"
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


def find_files_for_size(pattern: str, size: int) -> list[Path]:
    glob_pattern = pattern.replace("{size}", str(size)).replace("{id}", "*")
    return [Path(f) for f in sorted(glob.glob(glob_pattern))]


def compute_throughput(
    file: Path, batch_size: int, discard_start: float, discard_end: float
) -> float:
    df = pl.read_csv(file)
    if len(df) == 0:
        return 0.0

    min_ts = df["completion_timestamp_ns"].min()
    max_ts = df["completion_timestamp_ns"].max()
    start_cutoff = min_ts + int(discard_start * 1e9)
    end_cutoff = max_ts - int(discard_end * 1e9)

    df = df.filter(
        (pl.col("completion_timestamp_ns") >= start_cutoff)
        & (pl.col("completion_timestamp_ns") <= end_cutoff)
    )
    if len(df) == 0:
        return 0.0

    total_items = len(df) * batch_size
    duration_s = (
        df["completion_timestamp_ns"].max() - df["completion_timestamp_ns"].min()
    ) / 1e9
    if duration_s <= 0:
        return 0.0

    return total_items / duration_s / 1e6


def collect_throughputs(
    pattern: str,
    sizes: list[int],
    batch_size: int,
    discard_start: float,
    discard_end: float,
    label: str,
) -> list[float]:
    out = []
    for size in sizes:
        files = find_files_for_size(pattern, size)
        if not files:
            print(f"[{label}] Warning: no files for size {size}")
            out.append(0.0)
            continue
        instance = [
            compute_throughput(f, batch_size, discard_start, discard_end) for f in files
        ]
        instance = [t for t in instance if t > 0]
        if not instance:
            print(f"[{label}] Warning: no valid throughput for size {size}")
            out.append(0.0)
            continue
        total = sum(instance)
        print(
            f"[{label}] size {size}B: {total:.2f} M msgs/s (n={len(instance)} instances)"
        )
        out.append(total)
    return out


def main():
    args = parse_args()
    sizes = sorted(args.sizes)
    rows = args.rows
    two_columns = any(async_spec is not None for _, _, async_spec in rows)
    n_rows = len(rows)

    sns.set_theme(style="whitegrid")
    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"] = 42
    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.serif"] = ["Linux Libertine", "Linux Libertine O", "Libertinus Serif"]
    width = args.width if args.width is not None else 4.0
    fig = plt.figure(figsize=(width, 1.6 * n_rows), constrained_layout=True)
    row_axes, _ = make_row_axes(
        fig, [async_spec is not None for _, _, async_spec in rows], sharex=True
    )

    x_positions = range(len(sizes))
    left_title, right_title = args.column_titles
    for axes, (name, (pattern, batch_size), async_spec) in zip(row_axes, rows):
        panels = [(axes[0], pattern, batch_size, name, name.lower())]
        if async_spec is not None:
            async_pattern, async_batch_size = async_spec
            panels = [
                (axes[0], pattern, batch_size, f"{name} ({left_title})", name.lower()),
                (
                    axes[1],
                    async_pattern,
                    async_batch_size,
                    f"{name} ({right_title})",
                    f"{name.lower()} {right_title.lower()}",
                ),
            ]
        for ax, panel_pattern, panel_batch_size, title, stats_label in panels:
            throughputs = collect_throughputs(
                panel_pattern,
                sizes,
                panel_batch_size,
                args.discard_start,
                args.discard_end,
                stats_label,
            )
            ax.bar(x_positions, throughputs, color=PALETTE[0])
            ax.set_title(title)
            ax.yaxis.set_major_locator(plt.MaxNLocator(nbins=5))
        # Both panels of a pair keep their own y scale with the tick labels on
        # the left, like every other panel (the labels of the right-hand panel
        # therefore sit between the two panels).

    hide_inner_xticklabels(row_axes)
    fig.supylabel("Throughput (M msgs/s)")
    # The bottom row carries the size labels. In the two-column layout a
    # spanning (full-width) row carries its own as well: its bars do not
    # line up with the half-width panels below it. Half-width panels need
    # upright labels to keep nine of them apart; full-width ones use 45°.
    labelled_rows = [row_axes[-1]]
    if two_columns:
        labelled_rows += [row for row in row_axes[:-1] if len(row) == 1]
    for row in labelled_rows:
        rotation = 90 if two_columns and len(row) == 2 else 45
        for ax in row:
            ax.tick_params(labelbottom=True)
            ax.set_xticks(list(x_positions))
            ax.set_xticklabels([str(s) for s in sizes], rotation=rotation, ha="center")
    if two_columns:
        fig.supxlabel("Message size (bytes)")
    else:
        row_axes[-1][0].set_xlabel("Message size (bytes)")

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
