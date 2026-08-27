# Valkey Benchmarks

This directory contains the C++/libvalkey benchmark used as the Valkey
comparison baseline.

## Build

Requires CMake 3.16+, a C++17 compiler, and libvalkey's build dependencies.
For RDMA support, install the rdma-core development headers and libraries
(`librdmacm` and `libibverbs`). The CMake build fetches libvalkey via
`FetchContent`; if RDMA headers are missing, the binaries are built without
RDMA support and reject `rdma://` URLs at runtime.

Locally, the repository dev shell provides the toolchain (without RDMA
headers, so local builds are TCP-only):

```sh
nix develop ..#benchmarks
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Tests

Shell-based tests are registered with CTest. The CLI test needs no broker;
the integration tests start their own Valkey server in docker
(`valkey/valkey:9.1.0`, pulled on first use) and remove the container on
exit. CTest assigns each integration test its own host port (26380-26384).

```sh
nix develop ..#benchmarks
ctest --test-dir build --output-on-failure
```

## Broker Setup

The included results were measured against Valkey 9.1.0 built from source
with the RDMA module (`make BUILD_RDMA=module`): the published sync runs
with the `--io-threads 16` command below, the async runs with `--io-threads 1`.

Before launching the broker on `HOST2`, apply the kernel settings used for the
benchmark runs:

```sh
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w vm.overcommit_memory=1
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
```

Then start Valkey:

```sh
numactl -N 0 -m 0 ~/valkey-src/src/valkey-server \
    --port 6379 --dir ~/valkey-data \
    --save "" --appendonly no --loglevel warning --protected-mode no \
    --bind 10.0.0.2 127.0.0.1 --timeout 0 \
    --io-threads 16 --io-threads-do-reads yes --tcp-backlog 4096 \
    --hz 100 --dynamic-hz yes \
    --lazyfree-lazy-server-del yes --lazyfree-lazy-user-del yes \
    --lazyfree-lazy-expire yes --lazyfree-lazy-eviction yes \
    --loadmodule ~/valkey-src/src/valkey-rdma.so \
    --rdma-bind 10.0.0.3 --rdma-port 6379
```

`10.0.0.2`/`10.0.0.3` are placeholders —
substitute the broker's own addresses.

## Copy to Remote Machines

Before running a sweep, copy this directory to the hosts that execute the
sender/receiver, server, producer, or consumer processes. Run these commands
from the repository root and replace the host names as needed:

```sh
rsync -az --delete valkey/ HOST1:~/valkey/
rsync -az --delete valkey/ HOST3:~/valkey/
```

Then build the benchmark binaries on each remote host:

```sh
ssh HOST1 'cd ~/valkey && just -f just/rtt.just build && just -f just/throughput.just build'
ssh HOST3 'cd ~/valkey && just -f just/rtt.just build && just -f just/throughput.just build'
```

## Orchestrated Runs

`Makefile` drives the three-host benchmark setup:

- `HOST1`: sender/receiver or producer host.
- `HOST2`: Valkey broker host.
- `HOST3`: echo-server or consumer host.

The default `VALKEY_URL` is `rdma://$(HOST2):6379`.

`MODE` selects the send mode for all sweep targets (optional, default
`sync`; anything other than `sync`/`async` is rejected). It applies to the
rtt `send`/`server` roles and to throughput `produce`, and is folded into
the result filename tag: `sync` keeps the bare transport (`rtt_list_...`,
`throughput_list_...` — the published files), `async` appends `-async`
(`rtt_list-async_...`, `throughput_list-async_...`).

The sweep helpers are:

- `latency-vs-size`: run RTT measurements for multiple item sizes with one
  load level.
- `throughput-vs-size`: run producer/consumer throughput measurements for
  multiple item sizes.

Each helper derives the result filenames from its command-line variables and
copies generated CSVs into the local `results/` directory.

## Sweeps Used

The included Valkey latency results (`results/`, sync, io-threads 16) were
generated with (`10.0.0.3` standing in for the broker's RDMA address):

```sh
make -C valkey latency-vs-size \
    VALKEY_URL=rdma://10.0.0.3:6379 \
    SIZES="8 64 128 256 512" \
    SEND_CORES=0-31 SEND_CONC=32 \
    SRV_CORES=0-31 SRV_CONC=32 \
    RECV_CORES=32-63 RECV_CONC=32 \
    DUR=180s TPUT=0.01
```

The included Valkey throughput results (`results/`, sync, io-threads 16)
were generated with:

```sh
make -C valkey throughput-vs-size \
    VALKEY_URL=rdma://10.0.0.3:6379 \
    SIZES="8 64 128 256 512 1024 2048 4096 8192" \
    PROD_CORES=0-31 PROD_CONC=32 \
    CONS_CORES=0-31 CONS_CONC=32 \
    BATCHES=300 BSIZE=500 DUR=60s
```

The async results (the `-async` files in `results/`) use the
same sweeps with `MODE=async` against an `--io-threads 1` server; the
async throughput sweep uses per-size `BATCHES` values with `DUR=120s`
(the `_b*_bs500_d120s` files).
