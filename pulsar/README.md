# Pulsar Benchmarks

This directory contains the C++/pulsar-client-cpp benchmark used as the
Apache Pulsar comparison baseline.

## Build

Requires CMake 3.16+ and a C++17 compiler. The CMake build first looks for
an installed pulsar-client-cpp (`libpulsar` + `pulsar/Client.h`); otherwise
it fetches a pinned release (v4.2.0, override with
`-DPULSAR_CLIENT_CPP_TAG=...`) via `FetchContent` and builds the static
library. The source build needs protobuf (with `protoc`), openssl, libcurl,
zlib, zstd, snappy, and the boost headers.

Locally, the repository's nix shell provides everything:

```sh
nix develop ..#benchmarks
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On remote (non-nix) hosts install the dependencies with the distribution
package manager (e.g. `apt install libprotobuf-dev protobuf-compiler
libssl-dev libcurl4-openssl-dev zlib1g-dev libzstd-dev libsnappy-dev
libboost-dev`).

Note: cmake 4.x needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` at configure
time for some FetchContent dependencies, and the nix compiler wrapper strips
`-march=native` (`NIX_ENFORCE_NO_NATIVE`); on plain remote hosts the flag
applies normally.

## Tests

Shell-based tests are registered with CTest:

```sh
nix develop ..#benchmarks
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The integration tests start a Pulsar standalone broker in docker on ports
26650/28080 (override via `PULSAR_PORT` / `PULSAR_ADMIN_PORT`) and remove it
afterwards.

## Broker Setup

Start a Pulsar standalone broker on `HOST2`, from the release tarball (needs a JRE):

```sh
PULSAR_MEM="-Xms8g -Xmx8g -XX:MaxDirectMemorySize=8g" \
    numactl -N 0 -m 0 ~/apache-pulsar-4.0.5/bin/pulsar standalone -nfw -nss
```

Clients connect to `pulsar://HOST2:6650`; `init` talks to the admin REST API
on `http://HOST2:8080`.

The included results were measured against Pulsar 4.0.5 (release tarball,
OpenJDK 21) with three `conf/standalone.conf` changes from the defaults:
`advertisedAddress` set to the benchmark interface's address,
`managedLedgerNewEntriesCheckDelayInMillis=0`, and
`backlogQuotaCheckEnabled=false`.

`init` resets broker state before every run:

- `persistent`: `DELETE /admin/v2/persistent/public/default/<topic>?force=true`
  (a 404 for a missing topic is ignored), then
  `PUT /admin/v2/persistent/public/default/<topic>/subscription/bench` with
  JSON body `"earliest"`.
- `non-persistent`: connectivity check only.

## Copy to Remote Machines

Before running a sweep, copy this directory to the hosts that execute the
sender/receiver, server, producer, or consumer processes. Run these commands
from the repository root and replace the host names as needed:

```sh
rsync -az --delete pulsar/ HOST1:~/pulsar/
rsync -az --delete pulsar/ HOST3:~/pulsar/
```

Then build the benchmark binaries on each remote host:

```sh
ssh HOST1 'cd ~/pulsar && just -f just/rtt.just build && just -f just/throughput.just build'
ssh HOST3 'cd ~/pulsar && just -f just/rtt.just build && just -f just/throughput.just build'
```

## Orchestrated Runs

`Makefile` drives the three-host benchmark setup:

- `HOST1`: sender/receiver or producer host.
- `HOST2`: Pulsar broker host.
- `HOST3`: echo-server or consumer host.

The default `TRANSPORT` is `persistent`; override with
`TRANSPORT=non-persistent`. The rtt sweeps take an optional
`MODE=sync|async` (default `sync`) that selects the send mode of the sender
and echo server. Result filenames tag rtt runs with the bare transport for
sync (`rtt_persistent_...`, `rtt_non-persistent_...`) and with
`<transport>-async` for async (`rtt_persistent-async_...`); throughput runs
always carry `<transport>-<send-mode>` (`throughput_persistent-sync_...`,
`throughput_non-persistent-async_...`).

The sweep helpers are:

- `latency-vs-size`: run RTT measurements for multiple item sizes with one
  load level; optional `MODE=sync|async` (default `sync`).
- `throughput-vs-size`: run producer/consumer throughput measurements for
  multiple item sizes; requires `MODE=sync|async`.

Each helper derives the result filenames from its command-line variables and
copies generated CSVs into the local `results/` directory.

Example sweeps:

```sh
make -C pulsar latency-vs-size \
    PULSAR_URL=pulsar://10.0.0.2:6650 \
    PULSAR_ADMIN_URL=http://10.0.0.2:8080 \
    SIZES="8 64 128 256 512" \
    SEND_CORES=0-31 SEND_CONC=32 \
    SRV_CORES=0-31 SRV_CONC=32 \
    RECV_CORES=32-63 RECV_CONC=32 \
    DUR=180s TPUT=0.01

make -C pulsar throughput-vs-size \
    PULSAR_URL=pulsar://10.0.0.2:6650 \
    PULSAR_ADMIN_URL=http://10.0.0.2:8080 \
    MODE=async \
    SIZES="8 64 128 256 512 1024 2048 4096 8192" \
    PROD_CORES=0-31 PROD_CONC=32 \
    CONS_CORES=0-31 CONS_CONC=32 \
    BATCHES=300 BSIZE=500 DUR=120s
```
