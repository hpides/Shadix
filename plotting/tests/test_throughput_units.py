"""Tier 1 unit tests for plot_throughput_comparison: throughput math on
synthetic CSVs with known expected values."""

import pytest

import plot_throughput_comparison as ptc


def _write_tput_csv(path, timestamps):
    lines = ["task_id,batch_id,completion_timestamp_ns"]
    lines += [f"0,{i},{ts}" for i, ts in enumerate(timestamps)]
    path.write_text("\n".join(lines) + "\n")


def test_compute_throughput_known_value(tmp_path):
    # One batch completion every 0.5 s from 0 s to 10 s (21 rows).
    timestamps = [int(i * 0.5e9) for i in range(21)]
    f = tmp_path / "throughput_core-sync_s8_prod0-31c32_cons0-31c32_b300_bs500_d60s_id0.csv"
    _write_tput_csv(f, timestamps)

    # Discard 1 s at each end: rows at 1.0 s .. 9.0 s survive (17 rows).
    # duration = 9 s - 1 s = 8 s; items = 17 * 100.
    expected = 17 * 100 / 8.0 / 1e6
    got = ptc.compute_throughput(f, batch_size=100, discard_start=1.0, discard_end=1.0)
    assert got == pytest.approx(expected)


def test_compute_throughput_no_discard(tmp_path):
    # Batches at 0, 1, 2, 3, 4 s; batch_size 1000.
    timestamps = [int(i * 1e9) for i in range(5)]
    f = tmp_path / "throughput_js-async_s64_prod0-31c32_cons0-31c32_b300_bs500_d60s_id0.csv"
    _write_tput_csv(f, timestamps)

    expected = 5 * 1000 / 4.0 / 1e6
    got = ptc.compute_throughput(f, batch_size=1000, discard_start=0.0, discard_end=0.0)
    assert got == pytest.approx(expected)


def test_compute_throughput_empty_after_filter_is_zero(tmp_path):
    # Whole run spans 1 s; discarding 1 s from each end leaves nothing.
    timestamps = [0, int(0.5e9), int(1e9)]
    f = tmp_path / "throughput_persistent-sync_s8_prod0c1_cons0c1_b1_bs1_d1s_id0.csv"
    _write_tput_csv(f, timestamps)

    got = ptc.compute_throughput(f, batch_size=10, discard_start=1.0, discard_end=1.0)
    assert got == 0.0


def test_compute_throughput_zero_duration_is_zero(tmp_path):
    # A single surviving timestamp has zero duration.
    f = tmp_path / "throughput_core-async_s8_prod0c1_cons0c1_b1_bs1_d1s_id0.csv"
    _write_tput_csv(f, [int(1e9)])

    got = ptc.compute_throughput(f, batch_size=10, discard_start=0.0, discard_end=0.0)
    assert got == 0.0


def test_collect_throughputs_sums_instances(tmp_path, capsys):
    """The per-size value is the SUM over instance files (aggregate
    throughput of n concurrent producer instances), not a mean."""
    name = "throughput_core-sync_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"
    # id0: a completion every 0.5 s spanning 10 s -> 17 rows over 8 s
    # after discarding 1 s per end.
    _write_tput_csv(
        tmp_path / name.replace("{size}", "8").replace("{id}", "0"),
        [int(i * 0.5e9) for i in range(21)],
    )
    # id1: a completion every 1 s spanning 10 s -> 9 rows over 8 s.
    _write_tput_csv(
        tmp_path / name.replace("{size}", "8").replace("{id}", "1"),
        [int(i * 1e9) for i in range(11)],
    )

    expected = 17 * 100 / 8.0 / 1e6 + 9 * 100 / 8.0 / 1e6
    got = ptc.collect_throughputs(
        str(tmp_path / name),
        [8],
        batch_size=100,
        discard_start=1.0,
        discard_end=1.0,
        label="x",
    )
    assert got == [pytest.approx(expected)]
    assert "(n=2 instances)" in capsys.readouterr().out


def test_find_files_for_size_expands_size_and_id(tmp_path):
    name = "throughput_core-sync_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"
    for size in (8, 64):
        for run_id in (0, 1):
            concrete = name.replace("{size}", str(size)).replace("{id}", str(run_id))
            _write_tput_csv(tmp_path / concrete, [0, int(1e9)])

    pattern = str(tmp_path / name)
    files = ptc.find_files_for_size(pattern, 8)
    assert len(files) == 2
    assert all("_s8_" in f.name for f in files)


def test_find_files_for_size_keeps_list_and_list_async_apart(tmp_path):
    """Valkey pipelined runs are tagged `list-async`; the published sync
    files keep the bare `list` tag. Each pattern must select only its own
    family even when both sit in the same results directory."""
    sync = "throughput_list_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"
    pipelined = "throughput_list-async_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"
    for name in (sync, pipelined):
        for run_id in (0, 1):
            concrete = name.replace("{size}", "8").replace("{id}", str(run_id))
            _write_tput_csv(tmp_path / concrete, [0, int(1e9)])

    sync_files = ptc.find_files_for_size(str(tmp_path / sync), 8)
    pipelined_files = ptc.find_files_for_size(str(tmp_path / pipelined), 8)

    assert [f.name for f in sync_files] == [
        sync.replace("{size}", "8").replace("{id}", str(i)) for i in (0, 1)
    ]
    assert [f.name for f in pipelined_files] == [
        pipelined.replace("{size}", "8").replace("{id}", str(i)) for i in (0, 1)
    ]


@pytest.mark.parametrize(
    ("values", "expected"),
    [
        ([0.0, 6.0, 12.0, 18.0, 24.0], 0),
        ([0.0, 0.3, 0.6, 0.9, 1.2], 1),
        ([0.0, 0.04, 0.08, 0.12, 0.16], 2),
        ([0.0, 0.025, 0.05, 0.075, 0.1], 3),
        # A column mixing integer and 0.025-step ticks needs 3 decimals.
        ([0.0, 2.0, 4.0, 0.025, 0.05], 3),
    ],
)
def test_needed_decimals(values, expected):
    assert ptc.needed_decimals(values) == expected
