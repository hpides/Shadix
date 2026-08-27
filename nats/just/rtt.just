# RTT benchmark (C++ / nats.c).

rtt    := justfile_directory() + "/../build/rtt"
build_dir := justfile_directory() + "/../build"
src_dir   := justfile_directory() + "/.."

# Configure + build with cmake. Set NATS_C_TAG to override the version.
build:
    cmake -S {{src_dir}} -B {{build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{build_dir}} --target rtt -j

# Initialize broker state for both request and response subjects.
init url transport item_size req_subject resp_subject:
    {{rtt}} --url {{url}} --transport {{transport}} -s {{item_size}} \
        --req-subject {{req_subject}} --resp-subject {{resp_subject}} \
        init

# Run senders (cores is ';'-separated per instance).
# `send_mode` is sync|async; the Makefile folds it into the result filename tag.
send cores url transport item_size req_subject concurrency duration load send_mode:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    pids=""
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting sender $id on cores $core"
        taskset -c "$core" {{rtt}} --url {{url}} --transport {{transport}} -s {{item_size}} \
            --req-subject {{req_subject}} \
            send --id "$id" --concurrency {{concurrency}} --duration {{duration}} --load {{load}} \
            --send-mode {{send_mode}} &
        pids="$pids $!"
        id=$((id + 1))
    done
    fail=0
    for pid in $pids; do wait "$pid" || fail=1; done
    exit $fail

# Run receivers.
receive cores url transport item_size resp_subject concurrency duration output_dir:
    #!/bin/sh
    set -eu
    mkdir -p {{output_dir}}
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    pids=""
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting receiver $id on cores $core"
        taskset -c "$core" {{rtt}} --url {{url}} --transport {{transport}} -s {{item_size}} \
            --resp-subject {{resp_subject}} \
            receive --id "$id" --concurrency {{concurrency}} --duration {{duration}} \
            --output {{output_dir}}/rtt_{{transport}}_s{{item_size}}_c{{concurrency}}_d{{duration}}_r${id}.csv &
        pids="$pids $!"
        id=$((id + 1))
    done
    fail=0
    for pid in $pids; do wait "$pid" || fail=1; done
    exit $fail

# Run echo servers (`send_mode` as for `send`).
server cores url transport item_size req_subject resp_subject concurrency duration send_mode:
    #!/bin/sh
    set -eu
    cores="{{cores}}"
    instances=$(echo "$cores" | tr -cd ';' | wc -c)
    instances=$((instances + 1))
    pids=""
    id=0
    while [ "$id" -lt "$instances" ]; do
        core=$(echo "$cores" | cut -d';' -f$((id + 1)))
        echo "Starting server $id on cores $core"
        taskset -c "$core" {{rtt}} --url {{url}} --transport {{transport}} -s {{item_size}} \
            --req-subject {{req_subject}} --resp-subject {{resp_subject}} \
            server --id "$id" --concurrency {{concurrency}} --duration {{duration}} \
            --send-mode {{send_mode}} &
        pids="$pids $!"
        id=$((id + 1))
    done
    fail=0
    for pid in $pids; do wait "$pid" || fail=1; done
    exit $fail

