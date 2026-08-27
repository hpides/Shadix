# NATS Benchmarks

This directory contains the C++/nats.c benchmark used as the NATS comparison
baseline.

## Build

Requires CMake 3.16+ and a C++17 compiler. The CMake build fetches
[nats.c](https://github.com/nats-io/nats.c) (pinned via `NATS_C_TAG`,
default `v3.13.0`) via `FetchContent` and builds it statically without TLS
and without NATS Streaming.

Locally, the repository dev shell provides the toolchain and a `nats-server`:

```sh
nix develop ..#benchmarks
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Tests

Shell-based integration tests are registered with CTest and start their own
`nats-server`. CTest assigns each integration test its own port (24223-24232).

```sh
nix develop ..#benchmarks
ctest --test-dir build --output-on-failure
```

## Broker Setup

Start a JetStream-enabled NATS server on `HOST2`. The included results
were measured against nats-server v2.14.5 with all settings at their
defaults:

```sh
numactl -N 0 -m 0 nats-server -js -p 4222 -sd ~/nats-data
```

Start `core` sweeps against a server with no JetStream streams defined.

For the kernel settings used on the broker host, see `../valkey/README.md`
(somaxconn, overcommit, tcp_tw_reuse) — the same settings are applied for the
NATS runs.

## Copy to Remote Machines

Before running a sweep, copy this directory to the hosts that execute the
sender/receiver, server, producer, or consumer processes. Run these commands
from the repository root and replace the host names as needed:

```sh
rsync -az --delete nats/ HOST1:~/nats/
rsync -az --delete nats/ HOST3:~/nats/
```

Then build the benchmark binaries on each remote host:

```sh
ssh HOST1 'cd ~/nats && just -f just/rtt.just build && just -f just/throughput.just build'
ssh HOST3 'cd ~/nats && just -f just/rtt.just build && just -f just/throughput.just build'
```

## Orchestrated Runs

`Makefile` drives the three-host benchmark setup:

- `HOST1`: sender/receiver or producer host.
- `HOST2`: NATS broker host.
- `HOST3`: echo-server or consumer host.

The default `NATS_URL` is `nats://$(HOST2):4222` and the default `TRANSPORT`
is `core`; override with `TRANSPORT=js` for JetStream.

The sweep helpers are:

- `latency-vs-size`: run RTT measurements for multiple item sizes with one
  load level. It accepts an optional `MODE=sync|async` (default `sync`) that
  is passed to the sender and echo server as `--send-mode`. Sync
  runs keep the bare transport tag (`rtt_core_...`, `rtt_js_...`); async
  runs append `-async` (`rtt_core-async_...`, `rtt_js-async_...`).
- `throughput-vs-size`: run producer/consumer throughput measurements for
  multiple item sizes. Requires `MODE=sync|async`; the mode is
  always folded into the result filename tag (`throughput_core-async_...`,
  `throughput_js-sync_...`).

Each helper derives the result filenames from its command-line variables and
copies generated CSVs into the local `results/` directory.

Example sweeps:

```sh
make -C nats latency-vs-size \
    TRANSPORT=core \
    SIZES="8 64 128 256 512" \
    SEND_CORES=0-31 SEND_CONC=32 \
    SRV_CORES=0-31 SRV_CONC=32 \
    RECV_CORES=32-63 RECV_CONC=32 \
    DUR=180s TPUT=0.01

make -C nats throughput-vs-size \
    TRANSPORT=core MODE=async \
    SIZES="8 64 128 256 512 1024 2048 4096 8192" \
    PROD_CORES=0-31 PROD_CONC=32 \
    CONS_CORES=0-31 CONS_CONC=32 \
    BATCHES=300 BSIZE=500 DUR=180s
```
