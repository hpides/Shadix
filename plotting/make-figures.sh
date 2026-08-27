#!/usr/bin/env bash
# Regenerate the paper's figures in plots/ from the CSVs included in this
# repository: Figure 7 (latency_comparison_sync_async) and Figure 5
# (throughput_comparison_sync_async). Run from this directory inside the
# plotting environment:
#
#   nix develop          # from the repository root
#   cd plotting
#   ./make-figures.sh                                  # both figures
#   ./make-figures.sh latency_comparison_sync_async    # one figure
#
# README.md here documents the two plotting scripts; the root README's
# Measurement Environment section explains the file-name parameterizations.
# The RTT inputs are large (the Shadix CSVs are ~400 MB each), so the
# latency figure takes a few minutes.
set -euo pipefail
cd "$(dirname "$0")"
export MPLBACKEND=Agg

# --- input file sets -------------------------------------------------------
SHADIX_RTT=(); VALKEY_RTT=(); VALKEY_ARTT=()
NATS_RTT=(); NATS_ARTT=(); PULSAR_RTT=(); PULSAR_ARTT=()
for s in 8 64 128 256 512; do
    SHADIX_RTT+=("../shadix/results/rtt_s${s}_send25-29+30-34c5_srv25-34+35-44c10_recv35-39+40-44c5_d120s_l0.05.csv")
    VALKEY_RTT+=("../valkey/results/rtt_list_s${s}_send0-31c32_srv0-31c32_recv32-63c32_d180s_l0.01.csv")
    VALKEY_ARTT+=("../valkey/results/rtt_list-async_s${s}_send0-31c32_srv0-31c32_recv32-63c32_d180s_l0.01.csv")
    NATS_RTT+=(../nats/results/rtt_core_s${s}_*.csv)
    NATS_ARTT+=(../nats/results/rtt_core-async_s${s}_*.csv)
    PULSAR_RTT+=(../pulsar/results/rtt_non-persistent_s${s}_*.csv)
    PULSAR_ARTT+=(../pulsar/results/rtt_non-persistent-async_s${s}_*.csv)
done
for f in "${SHADIX_RTT[@]}" "${VALKEY_RTT[@]}" "${VALKEY_ARTT[@]}" \
         "${NATS_RTT[@]}" "${NATS_ARTT[@]}" "${PULSAR_RTT[@]}" "${PULSAR_ARTT[@]}"; do
    [ -f "$f" ] || { echo "MISSING RTT input: $f" >&2; exit 1; }
done

# Throughput patterns ({size}/{id} placeholders) and per-system batch sizes.
SHX="../shadix/results/throughput_s{size}_prod25-34+35-44c10_cons25-34+35-44c10_b*_bs4096_d15min_id{id}.csv"
VKX="../valkey/results/throughput_list_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv"
VPX="../valkey/results/throughput_list-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d120s_id{id}.csv"
NSX="../nats/results/throughput_core-sync_s{size}_prod0-31c32_cons0-31c32_b600_bs500_d120s_id{id}.csv"
NAX="../nats/results/throughput_core-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d180s_id{id}.csv"
PSX="../pulsar/results/throughput_non-persistent-sync_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d120s_id{id}.csv"
PAX="../pulsar/results/throughput_non-persistent-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d120s_id{id}.csv"

LAT="python plot_latency_comparison.py"
TPUT="python plot_throughput_comparison.py"
LOPTS=(--discard-start 4 --discard-end 4 --percentile 99.5)
TOPTS=(--sizes 8 64 128 256 512 1024 2048 4096 8192 --discard-start 4 --discard-end 4)

want() { [ $# -eq 0 ] && return 0 || true; for w in "$@"; do [ "$w" = "$FIG" ] && return 0; done; return 1; }
run_fig() { FIG=$1; shift; if want "${SELECT[@]}"; then echo "### $FIG"; "$@" -o "plots/$FIG"; fi; }
SELECT=("$@")

# --- Figure 7: RTT CDFs, sync (left) | async (right) -----------------------
run_fig latency_comparison_sync_async \
    $LAT --system Shadix "${SHADIX_RTT[@]}" \
         --system Valkey "${VALKEY_RTT[@]}" --async-system Valkey "${VALKEY_ARTT[@]}" \
         --system NATS "${NATS_RTT[@]}" --async-system NATS "${NATS_ARTT[@]}" \
         --system Pulsar "${PULSAR_RTT[@]}" --async-system Pulsar "${PULSAR_ARTT[@]}" "${LOPTS[@]}"

# --- Figure 5: throughput vs item size, sync (left) | async (right) --------
run_fig throughput_comparison_sync_async \
    $TPUT --system Shadix "$SHX" 4096 \
          --system Valkey "$VKX" 500 --async-system Valkey "$VPX" 500 \
          --system NATS "$NSX" 500 --async-system NATS "$NAX" 500 \
          --system Pulsar "$PSX" 500 --async-system Pulsar "$PAX" 500 "${TOPTS[@]}"

echo "done."
