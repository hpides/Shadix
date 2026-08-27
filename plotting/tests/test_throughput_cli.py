"""Tier 3 end-to-end CLI test for plot_throughput_comparison with three
synthetic systems."""

import os
import re
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "plot_throughput_comparison.py"

NAME = "throughput_{tag}_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"


def _write_tput_csv(path: Path, n_rows: int, step_ns: int) -> None:
    """Synthetic producer CSV with the real header; one task, evenly
    spaced batch completions."""
    lines = ["task_id,batch_id,completion_timestamp_ns"]
    for i in range(n_rows):
        lines.append(f"0,{i},{i * step_ns}")
    path.write_text("\n".join(lines) + "\n")


def test_throughput_cli_three_systems(tmp_path):
    # Batch sizes are chosen large enough that the expected totals have
    # significant digits at 2 decimals (2.12 and 0.85 M msgs/s), so the
    # stdout assertions below cannot degenerate to comparing "0.00".
    systems = {
        "valkey": ("list", 500000),
        "nats": ("core-async", 500000),
        "pulsar": ("persistent-sync", 200000),
    }
    patterns = {}
    for sysname, (tag, _) in systems.items():
        d = tmp_path / sysname / "results"
        d.mkdir(parents=True)
        for size in (8, 64):
            for run_id in (0, 1):
                name = (
                    NAME.replace("{tag}", tag)
                    .replace("{size}", str(size))
                    .replace("{id}", str(run_id))
                )
                # 21 completions, one every 0.5 s -> spans 10 s.
                _write_tput_csv(d / name, 21, int(0.5e9))
        patterns[sysname] = str(d / NAME.replace("{tag}", tag))

    out_base = tmp_path / "plots" / "throughput_comparison"
    out_base.parent.mkdir()

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "Valkey", patterns["valkey"], "500000",
            "--system", "NATS", patterns["nats"], "500000",
            "--system", "Pulsar", patterns["pulsar"], "200000",
            "--sizes", "8", "64",
            "--discard-start", "1",
            "--discard-end", "1",
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()

    # Known expected values: discarding 1 s per end leaves completions at
    # 1.0 s .. 9.0 s (17 rows) over an 8 s window per file, two files
    # (ids 0 and 1) summed per size.
    per_file = {bs: 17 * bs / 8.0 / 1e6 for bs in (500000, 200000)}
    stdout = result.stdout
    for label, bs in (("valkey", 500000), ("nats", 500000), ("pulsar", 200000)):
        total = 2 * per_file[bs]
        # Guard against a fixture regression that would make the numeric
        # comparison vacuous ("0.00" matches almost any aggregation bug).
        assert f"{total:.2f}" != "0.00"
        for size in (8, 64):
            line = f"[{label}] size {size}B: {total:.2f} M msgs/s (n=2 instances)"
            assert line in stdout, f"missing stats line {line!r} in:\n{stdout}"
    assert stdout.index("[valkey]") < stdout.index("[nats]") < stdout.index("[pulsar]")


def test_throughput_cli_warns_on_missing_size(tmp_path):
    d = tmp_path / "nats" / "results"
    d.mkdir(parents=True)
    name = NAME.replace("{tag}", "core-sync").replace("{size}", "8").replace("{id}", "0")
    _write_tput_csv(d / name, 21, int(0.5e9))
    pattern = str(d / NAME.replace("{tag}", "core-sync"))
    out_base = tmp_path / "throughput_missing"

    env = dict(os.environ, MPLBACKEND="Agg")
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--system", "NATS", pattern, "500",
            "--sizes", "8", "64",
            "-o", str(out_base),
        ],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stderr
    assert out_base.with_suffix(".svg").is_file()
    assert re.search(r"\[nats\] Warning: no files for size 64", result.stdout)
