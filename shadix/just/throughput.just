# Throughput Benchmark
#
# This benchmark runs across two machines:
# - Machine 1 (producer): runs producers
# - Machine 2 (consumer): runs consumers

throughput := justfile_directory() + "/../target/release/throughput"

# Build the throughput binary in release mode
build:
    cargo build --release --bin throughput --manifest-path {{justfile_directory()}}/../Cargo.toml

# Initialize shared memory regions
init item_size file offset producers consumers:
    {{throughput}} init \
        -s {{item_size}} --file {{file}} --offset {{offset}} \
        --producers {{producers}} \
        --consumers {{consumers}}

# Run producers
# cores: semicolon-separated core specs per instance (e.g., "0-3;4-7" for 2 instances)
# delay: duration in the future to start, e.g., "100ms", "1s" (for synchronization)
produce cores item_size file offset concurrency batches batch_size output_dir delay:
    #!/bin/sh
    set -eu
    mkdir -p {{output_dir}}
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))

    # Compute start time once for all instances using CLOCK_MONOTONIC_RAW
    start_time=$({{throughput}} start-time {{delay}})
    echo "Start time: $start_time (now + {{delay}})"

    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        output="{{output_dir}}/throughput_s{{item_size}}_b{{batches}}_bs{{batch_size}}_id${id}.csv"
        echo "Starting producer $id on cores $core"
        taskset -c "$core" {{throughput}} produce \
            -s {{item_size}} --file {{file}} --offset {{offset}} \
            --id "$id" \
            --concurrency {{concurrency}} \
            --batches {{batches}} \
            --batch-size {{batch_size}} \
            --output "$output" \
            --start-time "$start_time" &
        id=$((id + 1))
    done
    wait

# Run consumers
# cores: semicolon-separated core specs per instance (e.g., "0-3;4-7" for 2 instances)
consume cores item_size file offset concurrency duration:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting consumer $id on cores $core"
        taskset -c "$core" {{throughput}} consume \
            -s {{item_size}} --file {{file}} --offset {{offset}} \
            --id "$id" \
            --concurrency {{concurrency}} \
            --duration {{duration}} &
        id=$((id + 1))
    done
    wait

