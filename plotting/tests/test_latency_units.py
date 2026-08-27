"""Tier 1 unit tests for plot_latency_comparison: size extraction and
discard-window filtering."""

import pytest

import plot_latency_comparison as plc

SUFFIX = "send0-31c32_srv0-31c32_recv32-63c32_d180s_l0.01.csv"


@pytest.mark.parametrize(
    ("filename", "expected"),
    [
        # Shadix convention: no transport tag.
        (f"rtt_s8_{SUFFIX}", 8),
        (f"rtt_s512_{SUFFIX}", 512),
        # Valkey convention.
        (f"rtt_list_s8_{SUFFIX}", 8),
        (f"rtt_list_s1024_{SUFFIX}", 1024),
        # NATS conventions.
        (f"rtt_core_s8_{SUFFIX}", 8),
        (f"rtt_core_s64_{SUFFIX}", 64),
        (f"rtt_js_s8_{SUFFIX}", 8),
        (f"rtt_js_s256_{SUFFIX}", 256),
        # Pulsar conventions.
        (f"rtt_persistent_s8_{SUFFIX}", 8),
        (f"rtt_persistent_s128_{SUFFIX}", 128),
        (f"rtt_non-persistent_s8_{SUFFIX}", 8),
        (f"rtt_non-persistent_s8192_{SUFFIX}", 8192),
        # Async send mode (`rtt --send-mode async`): the transport tag
        # gains an `-async` suffix. non-persistent already contains a
        # hyphen, so the tag may hold several.
        (f"rtt_list-async_s8_{SUFFIX}", 8),
        (f"rtt_list-async_s512_{SUFFIX}", 512),
        (f"rtt_core-async_s8_{SUFFIX}", 8),
        (f"rtt_core-async_s64_{SUFFIX}", 64),
        (f"rtt_js-async_s8_{SUFFIX}", 8),
        (f"rtt_js-async_s256_{SUFFIX}", 256),
        (f"rtt_persistent-async_s8_{SUFFIX}", 8),
        (f"rtt_persistent-async_s128_{SUFFIX}", 128),
        (f"rtt_non-persistent-async_s8_{SUFFIX}", 8),
        (f"rtt_non-persistent-async_s8192_{SUFFIX}", 8192),
    ],
)
def test_extract_size_all_filename_conventions(filename, expected):
    assert plc.extract_size(filename) == expected


@pytest.mark.parametrize(
    "filename",
    [
        # Must not pick digits out of the core ranges / concurrency /
        # duration / load fields.
        f"rtt_core_s64_{SUFFIX}",
        f"rtt_non-persistent_s64_{SUFFIX}",
        f"rtt_core-async_s64_{SUFFIX}",
        f"rtt_non-persistent-async_s64_{SUFFIX}",
    ],
)
def test_extract_size_never_mis_extracts(filename):
    assert plc.extract_size(filename) == 64


def test_extract_size_rejects_unrelated_filename():
    with pytest.raises(ValueError):
        plc.extract_size("throughput_core-sync_s8_prod0-31c32_b300_bs500_d60s.csv")
    with pytest.raises(ValueError):
        plc.extract_size("notes.txt")


def _write_rtt_csv(path, rows):
    lines = ["send_time_ns,receive_time_ns"]
    lines += [f"{s},{r}" for s, r in rows]
    path.write_text("\n".join(lines) + "\n")


def test_load_and_filter_discard_window(tmp_path):
    # Send times every second from 0 s to 10 s; RTT is a constant 5 us.
    rows = [(int(t * 1e9), int(t * 1e9) + 5000) for t in range(11)]
    f = tmp_path / f"rtt_s8_{SUFFIX}"
    _write_rtt_csv(f, rows)

    df = plc.load_and_filter(f, discard_start=1.0, discard_end=1.0)

    # Rows with send_time_ns in [min + 1 s, max - 1 s] survive: 1 s .. 9 s.
    assert len(df) == 9
    assert df["send_time_ns"].min() == int(1e9)
    assert df["send_time_ns"].max() == int(9e9)
    assert df["rtt_us"].to_list() == pytest.approx([5.0] * 9)


def test_load_and_filter_zero_discard_keeps_everything(tmp_path):
    rows = [(int(t * 1e9), int(t * 1e9) + 1000) for t in range(5)]
    f = tmp_path / f"rtt_list_s8_{SUFFIX}"
    _write_rtt_csv(f, rows)

    df = plc.load_and_filter(f, discard_start=0.0, discard_end=0.0)
    assert len(df) == 5


def test_load_and_filter_header_only_csv_returns_empty(tmp_path):
    # A receiver that gets zero messages still writes the CSV header
    # (valkey/src/rtt.cpp). load_and_filter must return an empty frame
    # with the rtt_us column instead of raising.
    f = tmp_path / f"rtt_core_s8_{SUFFIX}"
    f.write_text("send_time_ns,receive_time_ns\n")

    df = plc.load_and_filter(f, discard_start=1.0, discard_end=1.0)
    assert len(df) == 0
    assert "rtt_us" in df.columns
