# Shadix Benchmarks

This directory contains the Rust Shadix implementation and the benchmark
binaries used in the evaluation.

## Build

```sh
cargo build --release
```

The benchmark binaries are built under `target/release/`:

- `throughput`: producer/consumer throughput benchmark.
- `rtt`: sender/server/receiver round-trip-time benchmark.

## Local Recipes

The recipes expect the Shadix checkout to be available on the remote machines
at `~/shadix` by default. Override `REMOTE_SHADIX` in `Makefile` if the remote
path differs.

## Copy to Remote Machines

Before running a sweep, copy this directory to both benchmark hosts. Run these
commands from the repository root and replace the host names as needed:

```sh
rsync -az --delete shadix/ HOST1:~/shadix/
rsync -az --delete shadix/ HOST2:~/shadix/
```

Then build the benchmark binaries on each remote host:

```sh
ssh HOST1 'cd ~/shadix && just -f just/rtt.just build && just -f just/throughput.just build'
ssh HOST2 'cd ~/shadix && just -f just/rtt.just build && just -f just/throughput.just build'
```

## Orchestrated Runs

`Makefile` drives two-host benchmark runs over SSH:

- `HOST1`: sender/receiver or producer host.
- `HOST2`: server or consumer host.

The sweep helpers are:

- `latency-vs-size`: run RTT measurements for multiple item sizes with one
  load level.
- `throughput-vs-size`: run producer/consumer throughput measurements for
  multiple item sizes.

Each helper derives the result filenames from its command-line variables and
copies generated CSVs into the local `results/` directory.

## Sweeps Used

The included Shadix latency results were generated with:

```sh
make -C shadix latency-vs-size \
    SIZES="8 64 128 256 512" \
    SEND_CORES=25-29+30-34 SEND_CONC=5 \
    SRV_CORES=25-34+35-44 SRV_CONC=10 \
    RECV_CORES=35-39+40-44 RECV_CONC=5 \
    DUR=120s TPUT=0.05
```

The included Shadix throughput results were generated with:

```sh
make -C shadix throughput-vs-size \
    SIZES="8" \
    PROD_CORES=25-34+35-44 PROD_CONC=10 \
    CONS_CORES=25-34+35-44 CONS_CONC=10 \
    BATCHES=131072 BSIZE=4096 DUR=15min

make -C shadix throughput-vs-size \
    SIZES="64 128 256 512 1024" \
    PROD_CORES=25-34+35-44 PROD_CONC=10 \
    CONS_CORES=25-34+35-44 CONS_CONC=10 \
    BATCHES=65536 BSIZE=4096 DUR=15min

make -C shadix throughput-vs-size \
    SIZES="2048" \
    PROD_CORES=25-34+35-44 PROD_CONC=10 \
    CONS_CORES=25-34+35-44 CONS_CONC=10 \
    BATCHES=32768 BSIZE=4096 DUR=15min

make -C shadix throughput-vs-size \
    SIZES="4096" \
    PROD_CORES=25-34+35-44 PROD_CONC=10 \
    CONS_CORES=25-34+35-44 CONS_CONC=10 \
    BATCHES=16384 BSIZE=4096 DUR=15min

make -C shadix throughput-vs-size \
    SIZES="8192" \
    PROD_CORES=25-34+35-44 PROD_CONC=10 \
    CONS_CORES=25-34+35-44 CONS_CONC=10 \
    BATCHES=8192 BSIZE=4096 DUR=15min
```

## Hardware Requirements

Reproducing the Shadix results requires two hosts sharing a CXL memory
device, exposed as a DAX device (default `/dev/dax0.0`) on both. The CXL
DAX device and offsets are configured at the top of `Makefile`:

- `CXL_DEVICE`
- `RTT_RESPONSE_OFFSET`

Note: the `just/` recipes do not propagate role-instance failures (bare
`wait`); the included results were validated by row counts.
