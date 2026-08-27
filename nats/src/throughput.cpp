// Throughput benchmark using C++ and nats.c.
//
// Subcommands:
//   start-time <offset>   prints CLOCK_MONOTONIC_RAW + offset (synchronization)
//   init                  resets broker state (recreates JetStream stream for js)
//   produce               batches × batch_size publishes per task, with
//                         per-batch completion timestamps written to CSV
//   consume               receive loop for `duration`
//
// Each worker owns one NATS connection.
//
// --send-mode sync  (default): one broker round trip per item (js PubAck /
//                   core publish + flush).
// --send-mode async: items are handed to the client library per item and
//                   settled with a flush_batch() barrier at each batch
//                   boundary (core: flush; js: wait for all pending
//                   PubAcks). The batch completion timestamp is taken after
//                   the barrier returns. A failed barrier is fatal: items
//                   may never have reached the broker, so no CSV or summary
//                   is produced and the process exits nonzero (same policy
//                   as the pulsar harness).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "args.hpp"
#include "monotonic.hpp"
#include "nats_transport.hpp"

using namespace natsbench;

namespace {

struct Common {
    std::string url;
    Transport transport;
    std::size_t item_size;
    std::string subject;
};

Common parse_common(const std::vector<std::string>& args) {
    Common c;
    c.url = find_opt(args, "url").value_or("nats://127.0.0.1:4222");
    c.transport = parse_transport(require_opt(args, "transport", "t"));
    c.item_size = parse_size(require_opt(args, "item-size", "s"));
    c.subject = find_opt(args, "subject").value_or("rb.tput");
    return c;
}

void cmd_init(const Common& c) {
    if (c.item_size == 0) throw std::runtime_error("item-size must be > 0");
    init_transport(c.url, c.transport, {c.subject});
    std::cout << "Initialized benchmark subject: subject=" << c.subject << '\n';
}

// Wait precisely until `target_ns`: coarse sleep, then spin the last ~10ms.
std::uint64_t wait_until(std::uint64_t target_ns) {
    auto now = monotonic_nanos();
    if (target_ns <= now) return now;
    std::uint64_t remaining = target_ns - now;
    if (remaining > 10'000'000ULL) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(remaining - 10'000'000ULL));
    }
    while ((now = monotonic_nanos()) < target_ns) {
        // spin
    }
    return now;
}

struct BatchMeasurement {
    std::size_t task_id;
    std::uint64_t batch_id;
    std::uint64_t completion_timestamp_ns;
};

void cmd_produce(const Common& c, const std::vector<std::string>& args) {
    // Validate all arguments (send-mode in particular) before any broker
    // contact so bad invocations fail fast with a clear message.
    auto id = parse_size(require_opt(args, "id", "i"));
    auto concurrency = parse_size(require_opt(args, "concurrency", "c"));
    auto batches = parse_u64(require_opt(args, "batches"));
    auto batch_size = parse_u64(require_opt(args, "batch-size"));
    auto start_time = parse_u64(require_opt(args, "start-time"));
    auto send_mode = parse_send_mode(find_opt(args, "send-mode").value_or("sync"));
    auto output = find_opt(args, "output", "o");

    std::vector<std::thread> threads;
    std::vector<std::vector<BatchMeasurement>> per_thread(concurrency);
    std::vector<std::uint64_t> task_starts(concurrency, 0);
    std::vector<std::exception_ptr> failures(concurrency);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            try {
                Producer producer(c.url, c.transport, c.subject);
                std::vector<std::uint8_t> payload(c.item_size, 0);

                const std::uint64_t task_start = wait_until(start_time);
                task_starts[t] = task_start;

                auto& measurements = per_thread[t];
                measurements.reserve(batches);

                for (std::uint64_t bi = 0; bi < batches; ++bi) {
                    for (std::uint64_t k = 0; k < batch_size; ++k) {
                        while (true) {
                            try {
                                if (send_mode == SendMode::sync_mode) {
                                    // Broker round trip per item (PubAck for
                                    // js, flush PING/PONG for core).
                                    producer.send_sync(payload.data(),
                                                       payload.size());
                                } else {
                                    // Client-buffered; settled at batch
                                    // boundary.
                                    producer.send_buffered(payload.data(),
                                                           payload.size());
                                }
                                break;
                            } catch (const std::exception& e) {
                                std::fprintf(stderr, "send error: %s\n",
                                             e.what());
                                std::this_thread::yield();
                            }
                        }
                    }
                    if (send_mode == SendMode::async_mode) {
                        // Barrier: the batch only counts as complete once
                        // every buffered send has reached the broker. A
                        // failed barrier means items may never have arrived,
                        // so it is fatal: the run's results would misreport
                        // delivered throughput (same policy as pulsar).
                        producer.flush_batch();
                    }
                    measurements.push_back({t, bi, monotonic_nanos()});
                }
            } catch (...) {
                failures[t] = std::current_exception();
            }
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t benchmark_end = monotonic_nanos();

    // Any task failure aborts the run (exit 1) before the summary/CSV are
    // produced: a partial run must never look like a complete one.
    for (std::size_t t = 0; t < concurrency; ++t) {
        if (!failures[t]) continue;
        try {
            std::rethrow_exception(failures[t]);
        } catch (const std::exception& e) {
            throw std::runtime_error("producer task " + std::to_string(t) +
                                     " failed: " + e.what());
        }
    }

    std::uint64_t earliest_start = UINT64_MAX;
    std::vector<BatchMeasurement> all;
    std::size_t total_meas = 0;
    for (auto& v : per_thread) total_meas += v.size();
    all.reserve(total_meas);
    for (std::size_t t = 0; t < concurrency; ++t) {
        if (task_starts[t] && task_starts[t] < earliest_start)
            earliest_start = task_starts[t];
        all.insert(all.end(), per_thread[t].begin(), per_thread[t].end());
    }

    const std::uint64_t total_items = batches * batch_size * concurrency;
    const double secs = (benchmark_end - earliest_start) / 1e9;
    std::cout << "Producer " << id << ": sent " << total_items << " items in "
              << secs << "s (" << (total_items / secs / 1e6) << " MOps/s)\n";

    if (output) {
        std::ofstream ofs(*output);
        if (!ofs) throw std::runtime_error("could not open " + *output);
        ofs << "task_id,batch_id,completion_timestamp_ns\n";
        for (const auto& m : all) {
            ofs << m.task_id << ',' << m.batch_id << ','
                << m.completion_timestamp_ns << '\n';
        }
        std::cout << "Wrote " << all.size() << " measurements to " << *output << '\n';
    }
}

void cmd_consume(const Common& c, const std::vector<std::string>& args) {
    auto id = parse_size(require_opt(args, "id", "i"));
    auto concurrency = parse_size(require_opt(args, "concurrency", "c"));
    auto duration_ns = parse_duration_ns(require_opt(args, "duration", "d"));

    const std::uint64_t benchmark_start = monotonic_nanos();
    const std::uint64_t deadline = benchmark_start + duration_ns;

    std::vector<std::thread> threads;
    std::vector<std::uint64_t> counts(concurrency, 0);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            Consumer consumer(c.url, c.transport, c.subject);
            std::vector<std::uint8_t> buf;
            std::uint64_t count = 0;
            while (true) {
                if (monotonic_nanos() > deadline) break;
                try {
                    if (consumer.recv(buf)) ++count;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "consume error: %s\n", e.what());
                    continue;
                }
            }
            counts[t] = count;
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t benchmark_end = monotonic_nanos();
    std::uint64_t total = 0;
    for (auto n : counts) total += n;
    const double secs = (benchmark_end - benchmark_start) / 1e9;
    std::cout << "Consumer " << id << ": received " << total << " items in "
              << secs << "s (" << (total / secs / 1e6) << " MOps/s)\n";
}

void cmd_start_time(const std::vector<std::string>& positional) {
    if (positional.empty()) {
        throw std::runtime_error("start-time requires an offset (e.g. 100ms, 1s)");
    }
    auto offset_ns = parse_duration_ns(positional[0]);
    std::cout << (monotonic_nanos() + offset_ns) << '\n';
}

void print_usage() {
    std::cerr <<
        "throughput SUBCOMMAND ...\n"
        "Subcommands:\n"
        "  start-time OFFSET\n"
        "  init     --url URL --transport core|js -s SIZE [--subject S]\n"
        "  produce  --url ... --id N --concurrency N --batches N --batch-size N\n"
        "           --start-time NS [--send-mode sync|async] [--output PATH]\n"
        "  consume  --url ... --id N --concurrency N --duration D\n";
}

}  // namespace

int main(int argc, char** argv) try {
    auto args = argv_to_vec(argc, argv);
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string subcmd = args[1];

    std::vector<std::string> rest(args.begin() + 2, args.end());

    if (subcmd == "start-time") {
        // first positional in `rest` is the offset
        std::vector<std::string> positional;
        for (const auto& a : rest) {
            if (a.empty() || a[0] != '-') positional.push_back(a);
        }
        cmd_start_time(positional);
    } else if (subcmd == "init") {
        cmd_init(parse_common(rest));
    } else if (subcmd == "produce") {
        cmd_produce(parse_common(rest), rest);
    } else if (subcmd == "consume") {
        cmd_consume(parse_common(rest), rest);
    } else {
        std::cerr << "unknown subcommand: " << subcmd << '\n';
        print_usage();
        return 2;
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
}
