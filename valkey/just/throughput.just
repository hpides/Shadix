# Throughput benchmark (C++ / libvalkey).

throughput := justfile_directory() + "/../build/throughput"
build_dir  := justfile_directory() + "/../build"
src_dir    := justfile_directory() + "/.."

build:
    cmake -S {{src_dir}} -B {{build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{build_dir}} --target throughput -j

init url transport item_size key:
    {{throughput}} init \
        --url {{url}} --transport {{transport}} -s {{item_size}} --key {{key}}

# Run producers. `delay` is the synchronization offset (e.g. "100ms", "1s").
# `send_mode` is sync|async; sync keeps the bare transport filename tag (the published throughput_list_... files), async appends -async.
produce cores url transport item_size key concurrency batches batch_size output_dir delay send_mode:
    #!/bin/sh
    set -eu
    mkdir -p {{output_dir}}
    case "{{send_mode}}" in
        sync)  tag="{{transport}}" ;;
        async) tag="{{transport}}-async" ;;
        *) echo "invalid send_mode '{{send_mode}}' (accepted values: sync, async)" >&2; exit 1 ;;
    esac
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    pids=""

    start_time=$({{throughput}} start-time {{delay}})
    echo "Start time: $start_time (now + {{delay}})"

    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        output="{{output_dir}}/throughput_${tag}_s{{item_size}}_b{{batches}}_bs{{batch_size}}_id${id}.csv"
        echo "Starting producer $id on cores $core"
        taskset -c "$core" {{throughput}} produce \
            --url {{url}} --transport {{transport}} -s {{item_size}} --key {{key}} \
            --id "$id" --concurrency {{concurrency}} \
            --batches {{batches}} --batch-size {{batch_size}} \
            --send-mode {{send_mode}} \
            --output "$output" --start-time "$start_time" &
        pids="$pids $!"
        id=$((id + 1))
    done
    fail=0
    for pid in $pids; do wait "$pid" || fail=1; done
    exit $fail

consume cores url transport item_size key concurrency duration:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    pids=""
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting consumer $id on cores $core"
        taskset -c "$core" {{throughput}} consume \
            --url {{url}} --transport {{transport}} -s {{item_size}} --key {{key}} \
            --id "$id" --concurrency {{concurrency}} --duration {{duration}} &
        pids="$pids $!"
        id=$((id + 1))
    done
    fail=0
    for pid in $pids; do wait "$pid" || fail=1; done
    exit $fail

