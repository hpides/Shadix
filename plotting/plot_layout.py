"""Row/column layout shared by the comparison plots.

A figure is a list of rows, one per system. A row is either a single
panel spanning the full width (a system measured in one mode only) or a
pair of panels, left and right (e.g. sync | async). In the two-column
layout the first spanning row can give up its right end to a legend slot,
so the legend does not add width to the figure.
"""

from __future__ import annotations

import argparse
import math

# Grid columns of the two-column layout: paired panels take 2 + 2, a
# spanning panel all 4, or 3 when the legend slot is carved out of it.
# The grid is deliberately coarse: constrained_layout charges a panel's
# tick-label margin to its first grid column, so columns must stay wider
# than those margins even at a 4-inch figure width.
_COLS = 4
_HALF = 2
_LEGEND_COLS = 1
LEGEND_AXES_LABEL = "legend"


def pair_rows(systems, async_systems, error):
    """Pair ``--system`` entries (left column) with ``--async-system``
    entries (right column) by label.

    ``systems`` and ``async_systems`` are lists of ``(label, payload)``;
    ``error`` is called with a message on an invalid combination (an
    async label without a left-column counterpart, or a label given
    twice). Returns ``[(label, payload, async_payload_or_None), ...]`` in
    ``--system`` order.
    """
    seen = set()
    for label, _ in systems:
        if label in seen:
            error(f"--system {label!r} given twice")
        seen.add(label)
    async_by_label = {}
    for label, payload in async_systems or []:
        if label in async_by_label:
            error(f"--async-system {label!r} given twice")
        async_by_label[label] = payload
    labels = [label for label, _ in systems]
    for label in async_by_label:
        if label not in labels:
            error(
                f"--async-system {label!r} has no matching --system {label!r} "
                "(the left column)"
            )
    return [(label, payload, async_by_label.get(label)) for label, payload in systems]


def positive_float(text: str) -> float:
    """argparse type for --width: a finite number > 0."""
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected a number, got {text!r}") from None
    if not (math.isfinite(value) and value > 0):
        raise argparse.ArgumentTypeError(f"must be a positive finite number, got {text!r}")
    return value


def make_row_axes(fig, paired: list[bool], sharex: bool = False, legend_slot: bool = False):
    """Create one row of axes per entry of ``paired``.

    Returns ``(rows, legend_ax)``. ``rows`` has one entry per row: ``[ax]``
    for a spanning row or ``[left_ax, right_ax]`` for a paired row. When
    no row is paired the grid has a single column, i.e. the classic
    stacked layout. With ``sharex`` every axis shares the x-axis of the
    first one. With ``legend_slot`` (two-column layout only) the first
    spanning row is shortened and an axis-less ``legend_ax`` (label
    ``LEGEND_AXES_LABEL``) is placed at its right end; ``legend_ax`` is
    ``None`` when there is no spanning row or no slot was requested.
    """
    two_columns = any(paired)
    gs = fig.add_gridspec(len(paired), _COLS if two_columns else 1)
    rows = []
    legend_ax = None
    first = None

    def add(spec):
        nonlocal first
        ax = fig.add_subplot(spec, sharex=first if sharex else None)
        if first is None:
            first = ax
        return ax

    for r, is_pair in enumerate(paired):
        if is_pair:
            rows.append([add(gs[r, :_HALF]), add(gs[r, _HALF:])])
        elif two_columns and legend_slot and legend_ax is None:
            rows.append([add(gs[r, : _COLS - _LEGEND_COLS])])
            legend_ax = fig.add_subplot(gs[r, _COLS - _LEGEND_COLS :], label=LEGEND_AXES_LABEL)
            legend_ax.set_axis_off()
        else:
            rows.append([add(gs[r, :])])
    return rows, legend_ax


def panel_axes(fig):
    """The data axes of ``fig`` (without the legend slot)."""
    return [ax for ax in fig.get_axes() if ax.get_label() != LEGEND_AXES_LABEL]


def hide_inner_xticklabels(rows) -> None:
    """Show x tick labels only on the bottom row (what ``sharex=True``
    does in ``plt.subplots``)."""
    for row in rows[:-1]:
        for ax in row:
            ax.tick_params(which="both", labelbottom=False)
