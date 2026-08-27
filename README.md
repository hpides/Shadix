# Shadix Paper Artifact

This repository contains the source code, benchmark harnesses, benchmark
results, and figures for the Shadix evaluation.

## Repository Layout

- `shadix/`: Rust implementation and benchmark binaries for Shadix.
- `valkey/`: C++/libvalkey benchmark used as the Valkey comparison baseline.
- `nats/`: C++/nats.c benchmark used as the NATS comparison baseline
  (core and JetStream transports).
- `pulsar/`: C++/pulsar-client-cpp benchmark used as the Apache Pulsar
  comparison baseline (persistent and non-persistent transports).
- `plotting/`: Python scripts, generated figures, and commands for
  reproducing the latency and throughput comparison plots.
- `flake.nix`, `flake.lock`: Nix dev shells — the default shell provides
  the plotting environment, `#benchmarks` the C++ harness toolchain.

All benchmark result CSVs used by the plotting scripts are included under
`shadix/results/`, `valkey/results/`,
`nats/results/`, and `pulsar/results/`. The CSVs are stored with Git LFS
(`git lfs pull` fetches them; ~4.0 GB).

## Measurement Environment

The Valkey, NATS, and Pulsar results were measured on three identical
servers (`HOST1`: sender/producer, `HOST2`: broker, `HOST3`: echo
server/consumer): 2 × AMD EPYC 7742 (128 cores, SMT off), 503 GB RAM,
Ubuntu 24.04.4 LTS, connected via 200 Gbit/s InfiniBand. NATS and Pulsar
communicate over TCP (IPoIB); Valkey uses its RDMA module on a second IB
port. Broker versions and configurations: Valkey 9.1.0, nats-server
v2.14.5, and Apache Pulsar 4.0.5 — each harness README gives the exact
server command, kernel settings, and configuration deltas. The Shadix
results come from a separate two-host setup sharing a CXL memory device,
as described in the paper.

Latency sweeps run 32 threads per role for 180 s at a total offered load
of 10 k msgs/s (encoded in the filenames as `_d180s_l0.01`). The async
throughput sweeps use per-size `BATCHES` values (≥ 30 s producer runtime)
and per-mode consumer durations — the `_b*_` and differing `_d...` parts
of the documented file patterns.

## Building

Each harness README documents its build: `cargo build --release` for
`shadix/`, and `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release &&
cmake --build build -j` for `valkey/`, `nats/`, and `pulsar/`. The C
client libraries (libvalkey, nats.c, pulsar-client-cpp) are fetched at
pinned tags via CMake `FetchContent`.

## Benchmark Harnesses

Each benchmark directory has a top-level `Makefile` for orchestrated multi-host
runs and a `just/` directory with the recipes executed on the remote machines.
Run `make help` in any benchmark directory for the sweep helpers and required
variables:

```sh
make -C shadix help
make -C valkey help
make -C nats help
make -C pulsar help
```

Before running a benchmark, copy the relevant benchmark directory to every
remote machine that executes its binaries and build it there — each harness
README has a "Copy to Remote Machines" section with the exact rsync and
build commands.

The makefiles provide sweep helpers for latency and throughput over
item sizes. They write local CSVs to the
harness's `results/` directory. Host names, broker locations, CXL device
paths, and remote result directories are set as variables at the top of each
makefile (the committed host defaults are placeholders) and can be overridden
on the command line.

## Plotting

The root `flake.nix` provides the Python environment used by the plotting
scripts:

```sh
nix develop
cd plotting
./make-figures.sh
```

This regenerates the paper's two figures
(`plotting/plots/latency_comparison_sync_async` and
`plotting/plots/throughput_comparison_sync_async`) from the included CSVs.

## License

Everything in this repository — code, benchmark data, and figures — is
released under the MIT license (see `LICENSE`). The third-party client
libraries (nats.c, libvalkey, pulsar-client-cpp) are not part of this
repository; they are fetched at build time from their upstream repositories
at pinned tags and remain under their own licenses.
