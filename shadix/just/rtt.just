# RTT Benchmark
#
# This benchmark runs across two machines:
# - Machine 1 (client): runs senders and receivers
# - Machine 2 (server): runs servers

rtt := justfile_directory() + "/../target/release/rtt"

# Build the rtt binary in release mode
build:
    cargo build --release --bin rtt --manifest-path {{justfile_directory()}}/../Cargo.toml

# Initialize shared memory regions
init item_size request_file request_offset response_file response_offset senders servers receivers:
    {{rtt}} -s {{item_size}} init \
        --request-file {{request_file}} \
        --request-offset {{request_offset}} \
        --response-file {{response_file}} \
        --response-offset {{response_offset}} \
        --senders {{senders}} \
        --servers {{servers}} \
        --receivers {{receivers}}

# Run senders
# cores: semicolon-separated core specs per instance (e.g., "0-3;4-7" for 2 instances)
send cores item_size file offset concurrency duration load:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting sender $id on cores $core"
        taskset -c "$core" {{rtt}} -s {{item_size}} send \
            --file {{file}} \
            --offset {{offset}} \
            --id "$id" \
            --concurrency {{concurrency}} \
            --duration {{duration}} \
            --load {{load}} &
        id=$((id + 1))
    done
    wait

# Run receivers
# cores: semicolon-separated core specs per instance (e.g., "0-3;4-7" for 2 instances)
receive cores item_size file offset concurrency duration output_dir:
    #!/bin/sh
    set -eu
    mkdir -p {{output_dir}}
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting receiver $id on cores $core"
        taskset -c "$core" {{rtt}} -s {{item_size}} receive \
            --file {{file}} \
            --offset {{offset}} \
            --id "$id" \
            --concurrency {{concurrency}} \
            --duration {{duration}} \
            --output {{output_dir}}/rtt_s{{item_size}}_c{{concurrency}}_d{{duration}}_r${id}.csv &
        id=$((id + 1))
    done
    wait

# Run servers
# cores: semicolon-separated core specs per instance (e.g., "0-3;4-7" for 2 instances)
server cores item_size request_file request_offset response_file response_offset concurrency duration:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting server $id on cores $core"
        taskset -c "$core" {{rtt}} -s {{item_size}} server \
            --request-file {{request_file}} \
            --request-offset {{request_offset}} \
            --response-file {{response_file}} \
            --response-offset {{response_offset}} \
            --id "$id" \
            --concurrency {{concurrency}} \
            --duration {{duration}} &
        id=$((id + 1))
    done
    wait

