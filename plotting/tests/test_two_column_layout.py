"""Sync | async two-column layout of both comparison plots.

``--async-system`` pairs a right-hand panel with the ``--system`` row of
the same label; rows without one span both columns; without any
``--async-system`` the classic single-column layout is unchanged.
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pytest

import plot_latency_comparison as plc
import plot_throughput_comparison as ptc
from plot_layout import LEGEND_AXES_LABEL, pair_rows, panel_axes

PLOTTING = Path(__file__).resolve().parent.parent
LAT_SCRIPT = PLOTTING / "plot_latency_comparison.py"
TPUT_SCRIPT = PLOTTING / "plot_throughput_comparison.py"

RTT_SUFFIX = "send0-31c32_srv0-31c32_recv32-63c32_d180s_l0.01.csv"
TPUT_NAME = "throughput_{tag}_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"


@pytest.fixture(autouse=True)
def _close_figures():
    yield
    plt.close("all")


def _write_rtt_csv(path: Path, rtt_ns: int) -> None:
    lines = ["send_time_ns,receive_time_ns"]
    for i in range(101):
        send = int(i * 0.1e9)
        lines.append(f"{send},{send + rtt_ns}")
    path.write_text("\n".join(lines) + "\n")


def _write_tput_csv(path: Path, n_rows: int, step_ns: int) -> None:
    lines = ["task_id,batch_id,completion_timestamp_ns"]
    for i in range(n_rows):
        lines.append(f"0,{i},{i * step_ns}")
    path.write_text("\n".join(lines) + "\n")


def _rtt_files(tmp_path: Path, tag: str, rtts: dict[int, int]) -> list[str]:
    out = []
    for size, rtt in rtts.items():
        f = tmp_path / f"rtt_{tag}s{size}_{RTT_SUFFIX}"
        _write_rtt_csv(f, rtt)
        out.append(str(f))
    return out


def _spans(ax):
    spec = ax.get_subplotspec()
    return spec.rowspan, spec.colspan


# --- pairing -----------------------------------------------------------------


def test_pair_rows_pairs_by_label_and_keeps_system_order():
    errors = []
    rows = pair_rows([("A", 1), ("B", 2)], [("B", 3)], errors.append)
    assert errors == []
    assert rows == [("A", 1, None), ("B", 2, 3)]


def test_pair_rows_without_async_entries():
    assert pair_rows([("A", 1)], None, lambda m: None) == [("A", 1, None)]


def test_pair_rows_rejects_duplicate_system_labels():
    errors = []
    pair_rows([("A", 1), ("A", 2)], [("A", 3)], errors.append)
    assert errors and "--system 'A' given twice" in errors[0]


def test_width_must_be_a_positive_finite_number(tmp_path, monkeypatch, capsys):
    nats = _rtt_files(tmp_path, "core_", {8: 50000})
    for bad in ("0", "-3", "nan", "wide"):
        monkeypatch.setattr(
            sys,
            "argv",
            [str(LAT_SCRIPT), "--system", "NATS", *nats, "--width", bad, "-o", str(tmp_path / "w")],
        )
        with pytest.raises(SystemExit) as exc:
            plc.main()
        assert exc.value.code == 2
        assert "--width" in capsys.readouterr().err


def test_pair_rows_rejects_async_without_sync_row_and_duplicates():
    errors = []
    pair_rows([("A", 1)], [("B", 2)], errors.append)
    assert errors and "no matching --system 'B'" in errors[0]

    errors = []
    pair_rows([("A", 1)], [("A", 2), ("A", 3)], errors.append)
    assert errors and "given twice" in errors[0]


# --- latency -----------------------------------------------------------------


def test_latency_two_column_layout(tmp_path, monkeypatch, capsys):
    shadix = _rtt_files(tmp_path, "", {8: 3000, 64: 4000})
    valkey_sync = _rtt_files(tmp_path, "list_", {8: 90000, 64: 95000})
    valkey_async = _rtt_files(tmp_path, "list-async_", {8: 100000, 64: 105000})
    out = tmp_path / "latency_two_columns"

    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(LAT_SCRIPT),
            "--system", "Shadix", *shadix,
            "--system", "Valkey", *valkey_sync,
            "--async-system", "Valkey", *valkey_async,
            "-o", str(out),
        ],
    )
    plc.main()

    fig = plt.gcf()
    axes = panel_axes(fig)
    assert [ax.get_title() for ax in axes] == ["Shadix", "Valkey (sync)", "Valkey (async)"]
    # Shadix has one mode and spans both columns minus the legend slot at
    # its right end (4-column grid); Valkey is sync | async.
    assert _spans(axes[0]) == (range(0, 1), range(0, 3))
    assert _spans(axes[1]) == (range(1, 2), range(0, 2))
    assert _spans(axes[2]) == (range(1, 2), range(2, 4))
    (legend_ax,) = [ax for ax in fig.get_axes() if ax.get_label() == LEGEND_AXES_LABEL]
    assert _spans(legend_ax) == (range(0, 1), range(3, 4))
    assert legend_ax.get_legend() is not None
    assert [t.get_text() for t in legend_ax.get_legend().get_texts()] == ["8 B", "64 B"]
    assert fig.legends == []  # no outside legend adding width
    # The legend's right edge lines up with the right edge of the right-hand
    # panels below it (both end at the grid's right edge).
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    legend_right = legend_ax.get_legend().get_window_extent(renderer).x1
    panel_right = axes[2].get_window_extent(renderer).x1
    assert abs(legend_right - panel_right) < 1.5  # pixels at 100 dpi
    # The legend is kept out of the layout computation (otherwise its
    # overhang becomes a grid-column margin that widens every sync | async
    # gap); it must still clear the Shadix panel, and the pair gap stays
    # small.
    assert not legend_ax.get_legend().get_in_layout()
    assert legend_ax.get_legend().get_window_extent(renderer).x0 > axes[0].get_window_extent(renderer).x1
    gap_in = (axes[2].get_window_extent(renderer).x0 - axes[1].get_window_extent(renderer).x1) / fig.dpi
    assert gap_in < 0.3
    # Column width by default, like the single-column figures.
    assert fig.get_figwidth() == 4.0
    assert fig.get_supxlabel() == "RTT (µs)"
    # Shared CDF scale: the right panel drops its y tick labels.
    fig.canvas.draw()
    assert all(t.get_visible() for t in axes[1].get_yticklabels())
    assert all(not t.get_visible() for t in axes[2].get_yticklabels())
    assert out.with_suffix(".svg").is_file() and out.with_suffix(".pdf").is_file()

    stdout = capsys.readouterr().out
    assert "[shadix] size 8B: min: 3.00 µs" in stdout
    assert "[valkey] size 8B: min: 90.00 µs" in stdout
    assert "[valkey async] size 8B: min: 100.00 µs" in stdout
    assert "[valkey async] size 64B: min: 105.00 µs" in stdout
    assert stdout.index("[shadix]") < stdout.index("[valkey]") < stdout.index("[valkey async]")


def test_latency_column_titles_and_width_are_configurable(tmp_path, monkeypatch):
    sync = _rtt_files(tmp_path, "list_", {8: 90000})
    async_ = _rtt_files(tmp_path, "list-async_", {8: 100000})
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(LAT_SCRIPT),
            "--system", "Valkey", *sync,
            "--async-system", "Valkey", *async_,
            "--column-titles", "per-item", "pipelined",
            "--width", "6.5",
            "-o", str(tmp_path / "titles"),
        ],
    )
    plc.main()
    fig = plt.gcf()
    assert [ax.get_title() for ax in panel_axes(fig)] == [
        "Valkey (per-item)",
        "Valkey (pipelined)",
    ]
    assert fig.get_figwidth() == 6.5
    # No spanning row -> no legend slot; the legend falls back to the
    # figure legend outside the axes.
    assert [ax for ax in fig.get_axes() if ax.get_label() == LEGEND_AXES_LABEL] == []
    assert len(fig.legends) == 1


def test_latency_single_column_layout_unchanged(tmp_path, monkeypatch):
    nats = _rtt_files(tmp_path, "core_", {8: 50000})
    pulsar = _rtt_files(tmp_path, "non-persistent_", {8: 60000})
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(LAT_SCRIPT),
            "--system", "NATS", *nats,
            "--system", "Pulsar", *pulsar,
            "-o", str(tmp_path / "single"),
        ],
    )
    plc.main()
    fig = plt.gcf()
    axes = fig.get_axes()
    assert [ax.get_title() for ax in axes] == ["NATS", "Pulsar"]
    assert all(_spans(ax)[1] == range(0, 1) for ax in axes)
    assert fig.get_figwidth() == 4.0
    assert axes[-1].get_xlabel() == "RTT (µs)"
    assert fig.get_supxlabel() == ""
    assert len(fig.legends) == 1


def test_latency_async_system_without_sync_row_is_an_error(tmp_path, monkeypatch, capsys):
    nats = _rtt_files(tmp_path, "core_", {8: 50000})
    pulsar_async = _rtt_files(tmp_path, "non-persistent-async_", {8: 60000})
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(LAT_SCRIPT),
            "--system", "NATS", *nats,
            "--async-system", "Pulsar", *pulsar_async,
            "-o", str(tmp_path / "bad"),
        ],
    )
    with pytest.raises(SystemExit) as exc:
        plc.main()
    assert exc.value.code == 2
    assert "no matching --system 'Pulsar'" in capsys.readouterr().err


# --- throughput --------------------------------------------------------------


def _tput_pattern(tmp_path: Path, tag: str, ids=(0, 1)) -> str:
    d = tmp_path / tag
    d.mkdir()
    for size in (8, 64):
        for run_id in ids:
            name = (
                TPUT_NAME.replace("{tag}", tag)
                .replace("{size}", str(size))
                .replace("{id}", str(run_id))
            )
            # 21 completions, one every 0.5 s -> spans 10 s.
            _write_tput_csv(d / name, 21, int(0.5e9))
    return str(d / TPUT_NAME.replace("{tag}", tag))


def test_throughput_two_column_layout(tmp_path, monkeypatch, capsys):
    valkey_sync = _tput_pattern(tmp_path, "list")
    valkey_async = _tput_pattern(tmp_path, "list-async")
    nats_sync = _tput_pattern(tmp_path, "core-sync", ids=(0,))
    out = tmp_path / "throughput_two_columns"

    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(TPUT_SCRIPT),
            "--system", "Valkey", valkey_sync, "500000",
            "--async-system", "Valkey", valkey_async, "500000",
            "--system", "NATS", nats_sync, "500000",
            "--sizes", "8", "64",
            "--discard-start", "1",
            "--discard-end", "1",
            "-o", str(out),
        ],
    )
    ptc.main()

    fig = plt.gcf()
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    axes = fig.get_axes()
    assert [ax.get_title() for ax in axes] == ["Valkey (sync)", "Valkey (async)", "NATS"]
    assert _spans(axes[0]) == (range(0, 1), range(0, 2))
    assert _spans(axes[1]) == (range(0, 1), range(2, 4))
    assert _spans(axes[2]) == (range(1, 2), range(0, 4))  # no legend slot here
    assert fig.get_figwidth() == 4.0
    # A full-width bottom row has room for 45° labels.
    assert all(t.get_rotation() == 45 for t in axes[2].get_xticklabels())
    assert fig.get_supxlabel() == "Message size (bytes)"
    # Both panels of a pair keep their own y scale, labelled on the left
    # like every other panel.
    for ax in (axes[0], axes[1]):
        tick = ax.yaxis.majorTicks[0]
        assert tick.label1.get_visible() and not tick.label2.get_visible()
        assert all(t.get_visible() for t in ax.get_yticklabels())
    # Shared x: only the bottom row carries the size tick labels.
    assert [t.get_text() for t in axes[2].get_xticklabels()] == ["8", "64"]
    assert all(not t.get_visible() for t in axes[0].get_xticklabels())
    assert all(not t.get_visible() for t in axes[1].get_xticklabels())
    assert out.with_suffix(".svg").is_file()

    # Discarding 1 s per end leaves 17 completions over 8 s per file.
    per_file = 17 * 500000 / 8.0 / 1e6
    stdout = capsys.readouterr().out
    for label, n in (("valkey", 2), ("valkey async", 2), ("nats", 1)):
        for size in (8, 64):
            line = f"[{label}] size {size}B: {n * per_file:.2f} M msgs/s (n={n} instances)"
            assert line in stdout, f"missing {line!r} in:\n{stdout}"
    assert stdout.index("[valkey]") < stdout.index("[valkey async]") < stdout.index("[nats]")


def test_throughput_spanning_row_above_pairs_keeps_its_size_labels(tmp_path, monkeypatch):
    """A spanning row above paired rows (Shadix over sync | async) shows
    its own size labels: its bars do not line up with the half-width
    panels, whose labels are upright at the bottom."""
    shadix = _tput_pattern(tmp_path, "shadix", ids=(0,))
    valkey_sync = _tput_pattern(tmp_path, "list", ids=(0,))
    valkey_async = _tput_pattern(tmp_path, "list-async", ids=(0,))
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(TPUT_SCRIPT),
            "--system", "Shadix", shadix, "500",
            "--system", "Valkey", valkey_sync, "500",
            "--async-system", "Valkey", valkey_async, "500",
            "--sizes", "8", "64",
            "-o", str(tmp_path / "span_top"),
        ],
    )
    ptc.main()
    fig = plt.gcf()
    fig.canvas.draw()
    top, left, right = fig.get_axes()
    assert top.get_title() == "Shadix" and _spans(top)[1] == range(0, 4)
    for ax, rotation in ((top, 45), (left, 90), (right, 90)):
        labels = ax.get_xticklabels()
        assert [t.get_text() for t in labels] == ["8", "64"]
        assert all(t.get_visible() for t in labels)
        assert all(t.get_rotation() == rotation for t in labels)


def test_throughput_single_column_layout_unchanged(tmp_path, monkeypatch):
    nats_sync = _tput_pattern(tmp_path, "core-sync", ids=(0,))
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(TPUT_SCRIPT),
            "--system", "NATS", nats_sync, "500",
            "--sizes", "8", "64",
            "-o", str(tmp_path / "single"),
        ],
    )
    ptc.main()
    fig = plt.gcf()
    (ax,) = fig.get_axes()
    assert ax.get_title() == "NATS"
    assert _spans(ax)[1] == range(0, 1)
    assert fig.get_figwidth() == 4.0
    assert ax.get_xlabel() == "Message size (bytes)"
    assert fig.get_supxlabel() == ""


def test_throughput_async_system_without_sync_row_is_an_error(tmp_path, monkeypatch, capsys):
    nats_sync = _tput_pattern(tmp_path, "core-sync", ids=(0,))
    pulsar_async = _tput_pattern(tmp_path, "non-persistent-async", ids=(0,))
    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(TPUT_SCRIPT),
            "--system", "NATS", nats_sync, "500",
            "--async-system", "Pulsar", pulsar_async, "500",
            "--sizes", "8", "64",
            "-o", str(tmp_path / "bad"),
        ],
    )
    with pytest.raises(SystemExit) as exc:
        ptc.main()
    assert exc.value.code == 2
    assert "no matching --system 'Pulsar'" in capsys.readouterr().err
