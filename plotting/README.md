# Generating Plots

This directory contains the scripts used to regenerate the paper's
comparison figures from the CSV files in `../shadix/results/`,
`../valkey/results/`, `../nats/results/`, and `../pulsar/results/`.

The nix flake lives at the project root. From the repository root, enter the
plotting environment with `nix develop`, then change into this directory before
running the commands below.

Plots are written as both PDF and SVG files.

## Sync | async layout

Both scripts accept `--async-system` with the same arguments as `--system`.
It adds a right-hand panel to the row of the `--system` entry with the same
label, so a system measured in both send modes is shown as sync (left) |
async (right) in one row, while a system given only with `--system` (e.g.
Shadix) keeps a single panel spanning both columns. Rows stay in `--system`
order. Paired panels are titled `<label> (sync)` and `<label> (async)`;
`--column-titles LEFT RIGHT` renames the suffixes, and `--width` overrides
the 4 in figure width. The right-hand panel's stats lines are prefixed
`[<label> <RIGHT>]` in lower case, i.e. `[<label> async]` with the default
column titles. An `--async-system`
whose label has no `--system` row is an error.

## Latency

Each `--system` takes a display label followed by that system's RTT CSV files.
The message size is parsed from the filenames, which cover all harness
conventions (`rtt_s8_...`, `rtt_list_s8_...`, `rtt_core_s8_...`,
`rtt_non-persistent_s8_...`). Runs made with `rtt --send-mode async` append
`-async` to the transport tag (`rtt_list-async_s8_...`,
`rtt_core-async_s8_...`, `rtt_non-persistent-async_s8_...`); the
parser covers that family as well, and such files plot exactly like their
sync counterparts — pass e.g. `../valkey/results/rtt_list-async_s*` in
place of `rtt_list_s*`.
`--xlim MIN MAX` optionally fixes the x-axis (µs) of every subplot;
otherwise `--percentile` sets each subplot's x-axis max from its own data.

To generate a latency plot for all four systems (single send mode), run

```bash
python plot_latency_comparison.py \
    --system Shadix ../shadix/results/rtt_s* \
    --system Valkey ../valkey/results/rtt_list_s* \
    --system NATS ../nats/results/rtt_core_s* \
    --system Pulsar ../pulsar/results/rtt_non-persistent_s* \
    --discard-start 4 --discard-end 4 --percentile 99.5 \
    -o plots/my_latency_comparison
```

Each system prints one stats line per size, in the order the `--system` flags
were given. For the Shadix and Valkey results the output starts with

```
[shadix] size 8B: min: 1.82 µs, median: 3.21 µs, p99: 7.29 µs
[shadix] size 64B: min: 2.89 µs, median: 4.42 µs, p99: 8.45 µs
[shadix] size 128B: min: 2.89 µs, median: 4.49 µs, p99: 8.62 µs
[shadix] size 256B: min: 2.88 µs, median: 4.51 µs, p99: 8.63 µs
[shadix] size 512B: min: 3.00 µs, median: 4.66 µs, p99: 9.14 µs
[valkey] size 8B: min: 75.71 µs, median: 99.62 µs, p99: 166.95 µs
[valkey] size 64B: min: 76.12 µs, median: 100.18 µs, p99: 165.52 µs
[valkey] size 128B: min: 76.20 µs, median: 100.51 µs, p99: 173.71 µs
[valkey] size 256B: min: 77.56 µs, median: 100.16 µs, p99: 175.59 µs
[valkey] size 512B: min: 81.24 µs, median: 102.97 µs, p99: 182.10 µs
```

followed by `[nats]` and `[pulsar]` lines in the same format (their results
are included as well).

The paper's Figure 7 is the sync | async version, which adds the
`rtt --send-mode async` files as right-hand panels:

```bash
python plot_latency_comparison.py \
    --system Shadix ../shadix/results/rtt_s* \
    --system Valkey ../valkey/results/rtt_list_s* \
    --async-system Valkey ../valkey/results/rtt_list-async_s* \
    --system NATS ../nats/results/rtt_core_s* \
    --async-system NATS ../nats/results/rtt_core-async_s* \
    --system Pulsar ../pulsar/results/rtt_non-persistent_s* \
    --async-system Pulsar ../pulsar/results/rtt_non-persistent-async_s* \
    --discard-start 4 --discard-end 4 --percentile 99.5 \
    -o plots/latency_comparison_sync_async
```

## Throughput

Each `--system` takes three values: a display name, a quoted file pattern
with `{size}` and `{id}` placeholders, and the producer batch size used
for that system's runs (`bs` in the filename). `{id}` matches any run id; throughput is summed over the matching
instances per size.

To generate a throughput plot for all four systems (single send mode), run

```bash
python plot_throughput_comparison.py \
    --system Shadix "../shadix/results/throughput_s{size}_prod25-34+35-44c10_cons25-34+35-44c10_b*_bs4096_d15min_id{id}.csv" 4096 \
    --system Valkey "../valkey/results/throughput_list_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv" 500 \
    --system NATS "../nats/results/throughput_core-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d180s_id{id}.csv" 500 \
    --system Pulsar "../pulsar/results/throughput_non-persistent-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d120s_id{id}.csv" 500 \
    --sizes 8 64 128 256 512 1024 2048 4096 8192 \
    --discard-start 4 --discard-end 4 \
    -o plots/my_throughput_comparison
```

The NATS and Pulsar filename tags combine the transport with the send mode
(`core-sync`, `core-async`, `non-persistent-sync`, `non-persistent-async`);
pick the tag matching the runs to plot. The Valkey tag is `list` for the published sync runs
(`throughput_list_s{size}_..._id{id}.csv`) and `list-async` for async
runs (`throughput_list-async_s{size}_..._id{id}.csv`); Shadix files carry no
transport tag.

The paper's Figure 5 is the sync | async version:

```bash
python plot_throughput_comparison.py \
    --system Shadix "../shadix/results/throughput_s{size}_prod25-34+35-44c10_cons25-34+35-44c10_b*_bs4096_d15min_id{id}.csv" 4096 \
    --system Valkey "../valkey/results/throughput_list_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d60s_id{id}.csv" 500 \
    --async-system Valkey "../valkey/results/throughput_list-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d120s_id{id}.csv" 500 \
    --system NATS "../nats/results/throughput_core-sync_s{size}_prod0-31c32_cons0-31c32_b600_bs500_d120s_id{id}.csv" 500 \
    --async-system NATS "../nats/results/throughput_core-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d180s_id{id}.csv" 500 \
    --system Pulsar "../pulsar/results/throughput_non-persistent-sync_s{size}_prod0-31c32_cons0-31c32_b300_bs500_d120s_id{id}.csv" 500 \
    --async-system Pulsar "../pulsar/results/throughput_non-persistent-async_s{size}_prod0-31c32_cons0-31c32_b*_bs500_d120s_id{id}.csv" 500 \
    --sizes 8 64 128 256 512 1024 2048 4096 8192 \
    --discard-start 4 --discard-end 4 \
    -o plots/throughput_comparison_sync_async
```

For the Shadix and Valkey results included here the output is

```
[shadix] size 8B: 25.62 M msgs/s (n=2 instances)
[shadix] size 64B: 11.05 M msgs/s (n=2 instances)
[shadix] size 128B: 10.98 M msgs/s (n=2 instances)
[shadix] size 256B: 9.14 M msgs/s (n=2 instances)
[shadix] size 512B: 8.05 M msgs/s (n=2 instances)
[shadix] size 1024B: 7.12 M msgs/s (n=2 instances)
[shadix] size 2048B: 5.49 M msgs/s (n=2 instances)
[shadix] size 4096B: 3.17 M msgs/s (n=2 instances)
[shadix] size 8192B: 1.66 M msgs/s (n=2 instances)
[valkey] size 8B: 0.11 M msgs/s (n=1 instances)
[valkey] size 64B: 0.10 M msgs/s (n=1 instances)
[valkey] size 128B: 0.09 M msgs/s (n=1 instances)
[valkey] size 256B: 0.09 M msgs/s (n=1 instances)
[valkey] size 512B: 0.09 M msgs/s (n=1 instances)
[valkey] size 1024B: 0.09 M msgs/s (n=1 instances)
[valkey] size 2048B: 0.08 M msgs/s (n=1 instances)
[valkey] size 4096B: 0.08 M msgs/s (n=1 instances)
[valkey] size 8192B: 0.07 M msgs/s (n=1 instances)
```

again followed by `[nats]` and `[pulsar]` lines for the included NATS and
Pulsar results.

## Paper Figures

`plots/` contains exactly the paper's two figures; `./make-figures.sh`
regenerates both from the included CSVs (run it from this directory inside
the nix shell, optionally passing a figure name to regenerate just one):

| paper figure | file in `plots/` |
|---|---|
| Figure 5 (throughput vs item size, sync \| async) | `throughput_comparison_sync_async.{pdf,svg}` |
| Figure 7 (RTT CDFs 8–512 B, sync \| async) | `latency_comparison_sync_async.{pdf,svg}` |

## Tests

From the repository root:

```bash
MPLBACKEND=Agg nix develop -c python -m pytest plotting/tests -q
```
