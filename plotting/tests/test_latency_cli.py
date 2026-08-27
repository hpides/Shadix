"""Tier 2 end-to-end CLI test for plot_latency_comparison with three
synthetic systems."""

import os
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pytest

import plot_latency_comparison as plc

SCRIPT = Path(__file__).resolve().parent.parent / "plot_latency_comparison.py"


@pytest.fixture(autouse=True)
def _close_figures():
    yield
    plt.close("all")

SUFFIX = "send0-31c32_srv0-31c32_recv32-63c32_d180s_l0.01.csv"


def _write_rtt_csv(path: Path, rtt_ns: int) -> None:
    """Synthetic RTT CSV spanning 10 s with the real header."""
    lines = ["send_time_ns,receive_time_ns"]
    for i in range(101):
        send = int(i * 0.1e9)
        lines.append(f"{send},{send + rtt_ns}")
    path.write_text("\n".join(lines) + "\n")


def test_latency_cli_three_systems(tmp_path):
    shadix_dir = tmp_path / "shadix" / "results"
    valkey_dir = tmp_path / "valkey" / "results"
    nats_dir = tmp_path / "nats" / "results"
    for d in (shadix_dir, valkey_dir, nats_dir):
        d.mkdir(parents=True)

    shadix_files = []
    for size, rtt in ((8, 3000), (64, 4000)):
        f = shadix_dir / f"rtt_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        shadix_files.append(str(f))

    valkey_files = []
    for size, rtt in ((8, 90000), (64, 95000)):
        f = valkey_dir / f"rtt_list_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        valkey_files.append(str(f))

    nats_files = []
    for size, rtt in ((8, 50000), (64, 55000)):
        f = nats_dir / f"rtt_core_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        nats_files.append(str(f))

    out_base = tmp_path / "plots" / "latency_comparison"
    out_base.parent.mkdir()

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "Shadix", *shadix_files,
            "--system", "Valkey", *valkey_files,
            "--system", "NATS", *nats_files,
            "--discard-start", "1",
            "--discard-end", "1",
            "--percentile", "99.5",
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()

    # The printed stats lines are preserved, one per system and size, in
    # the order the systems were given.
    stdout = result.stdout
    for label, rtt_us in (
        ("[shadix] size 8B: min: 3.00 µs, median: 3.00 µs, p99: 3.00 µs", None),
        ("[shadix] size 64B: min: 4.00 µs, median: 4.00 µs, p99: 4.00 µs", None),
        ("[valkey] size 8B: min: 90.00 µs, median: 90.00 µs, p99: 90.00 µs", None),
        ("[valkey] size 64B: min: 95.00 µs, median: 95.00 µs, p99: 95.00 µs", None),
        ("[nats] size 8B: min: 50.00 µs, median: 50.00 µs, p99: 50.00 µs", None),
        ("[nats] size 64B: min: 55.00 µs, median: 55.00 µs, p99: 55.00 µs", None),
    ):
        assert label in stdout, f"missing stats line {label!r} in:\n{stdout}"
    assert stdout.index("[shadix]") < stdout.index("[valkey]") < stdout.index("[nats]")


def test_latency_cli_empty_csv_warns_and_still_plots(tmp_path):
    """A header-only CSV (receiver got zero messages) must not abort the
    plot; it takes the 'no RTT samples' warning path instead."""
    good = tmp_path / f"rtt_core_s8_{SUFFIX}"
    _write_rtt_csv(good, 5000)
    empty = tmp_path / f"rtt_core_s64_{SUFFIX}"
    empty.write_text("send_time_ns,receive_time_ns\n")
    out_base = tmp_path / "latency_empty"

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "NATS", str(good), str(empty),
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()
    assert (
        "[nats] size 64B: Warning: no RTT samples after filtering" in result.stdout
    ), result.stdout
    assert "[nats] size 8B: min: 5.00" in result.stdout


def test_latency_cli_async_tag_files_plot_like_sync(tmp_path):
    """Files from `rtt --send-mode async` carry a `-async` transport tag
    (`rtt_list-async_s8_...`, `rtt_non-persistent-async_s8_...`). They
    must parse and plot exactly like their sync counterparts."""
    valkey_files = []
    for size, rtt in ((8, 7000), (64, 8000)):
        f = tmp_path / f"rtt_list-async_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        valkey_files.append(str(f))
    pulsar_files = []
    for size, rtt in ((8, 9000), (64, 11000)):
        f = tmp_path / f"rtt_non-persistent-async_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        pulsar_files.append(str(f))
    out_base = tmp_path / "latency_async"

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "Valkey", *valkey_files,
            "--system", "Pulsar", *pulsar_files,
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()
    stdout = result.stdout
    for line in (
        "[valkey] size 8B: min: 7.00 µs, median: 7.00 µs, p99: 7.00 µs",
        "[valkey] size 64B: min: 8.00 µs, median: 8.00 µs, p99: 8.00 µs",
        "[pulsar] size 8B: min: 9.00 µs, median: 9.00 µs, p99: 9.00 µs",
        "[pulsar] size 64B: min: 11.00 µs, median: 11.00 µs, p99: 11.00 µs",
    ):
        assert line in stdout, f"missing stats line {line!r} in:\n{stdout}"


def test_latency_cli_global_xlim(tmp_path):
    f = tmp_path / f"rtt_core_s8_{SUFFIX}"
    _write_rtt_csv(f, 5000)
    out_base = tmp_path / "latency_xlim"

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "NATS", str(f),
            "--xlim", "0", "100",
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()


def test_latency_cli_xlim_applied_to_axes(tmp_path, monkeypatch):
    """--xlim must actually reach every subplot, not just parse."""
    files = []
    for size, rtt in ((8, 3000), (64, 4000)):
        f = tmp_path / f"rtt_core_s{size}_{SUFFIX}"
        _write_rtt_csv(f, rtt)
        files.append(str(f))
    out_base = tmp_path / "latency_xlim_axes"

    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(SCRIPT),
            "--system", "NATS", *files,
            "--system", "NATS2", *files,
            "--xlim", "0", "42",
            "-o", str(out_base),
        ],
    )
    plc.main()

    axes = plt.gcf().get_axes()
    assert len(axes) == 2
    for ax in axes:
        assert ax.get_xlim() == (0.0, 42.0)


def test_latency_cli_percentile_sets_xmax(tmp_path, monkeypatch):
    """--percentile must set each subplot's x-max to that percentile of
    its own RTT data (constant 5 us RTT -> any percentile is 5.0)."""
    f = tmp_path / f"rtt_core_s8_{SUFFIX}"
    _write_rtt_csv(f, 5000)
    out_base = tmp_path / "latency_percentile_axes"

    monkeypatch.setattr(
        sys,
        "argv",
        [
            str(SCRIPT),
            "--system", "NATS", str(f),
            "--percentile", "99",
            "-o", str(out_base),
        ],
    )
    plc.main()

    (ax,) = plt.gcf().get_axes()
    lo, hi = ax.get_xlim()
    assert lo == 0.0
    assert hi == pytest.approx(5.0)
