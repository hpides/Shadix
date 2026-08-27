// Round-trip-time benchmark using C++ and nats.c.
//
// Methodology:
//
// - Three roles connected through a dedicated NATS broker:
//     `send`    publishes timestamped requests to the request subject
//     `server`  consumes requests and re-publishes the payload verbatim to
//               the response subject
//     `receive` consumes responses, reads the embedded timestamp,
//               and writes (send_time_ns, receive_time_ns) rows to CSV.
// - Sender and receiver must run on the same host (RTT is the difference
//   of two CLOCK_MONOTONIC_RAW reads).
// - item_size must be >= 8 (first 8 bytes carry the timestamp).
// - One NATS connection per thread — handles are not shared across threads.
// - `--send-mode` (send and server only): `sync` (default) waits for a broker
//   round trip per message (core: publish + flush; js: PubAck). `async` hands
//   each message to the client library and settles once per thread after the
//   duration loop (flush_batch), so nothing is left unsent on exit. A failed
//   settle is fatal: exit 1 and no summary, same policy as throughput.cpp.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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
    std::string req_subject;
    std::string resp_subject;
};

Common parse_common(const std::vector<std::string>& args) {
    Common c;
    c.url = find_opt(args, "url").value_or("nats://127.0.0.1:4222");
    c.transport = parse_transport(require_opt(args, "transport", "t"));
    c.item_size = parse_size(require_opt(args, "item-size", "s"));
    if (c.item_size < 8) {
        throw std::runtime_error("item-size must be >= 8");
    }
    c.req_subject = find_opt(args, "req-subject").value_or("rb.rtt.req");
    c.resp_subject = find_opt(args, "resp-subject").value_or("rb.rtt.resp");
    return c;
}

void cmd_init(const Common& c) {
    init_transport(c.url, c.transport, {c.req_subject, c.resp_subject});
    std::cout << "Initialized benchmark subjects: req=" << c.req_subject
              << " resp=" << c.resp_subject << '\n';
}

void busy_wait_until(std::uint64_t target_ns) {
    while (monotonic_nanos() < target_ns) {
        std::this_thread::yield();
    }
}

// Any task failure aborts the run (exit 1) before the summary is printed: a
// partial run must never look like a complete one.
void rethrow_task_failure(const char* role,
                          const std::vector<std::exception_ptr>& failures) {
    for (std::size_t t = 0; t < failures.size(); ++t) {
        if (!failures[t]) continue;
        try {
            std::rethrow_exception(failures[t]);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(role) + " task " +
                                     std::to_string(t) + " failed: " + e.what());
        }
    }
}

void cmd_send(const Common& c, const std::vector<std::string>& args) {
    auto id = parse_size(require_opt(args, "id", "i"));
    auto concurrency = parse_size(require_opt(args, "concurrency", "c"));
    auto duration_ns = parse_duration_ns(require_opt(args, "duration", "d"));
    auto load = parse_f64(require_opt(args, "load", "l"));
    // Validated before any broker contact so a bad value fails fast.
    auto send_mode = parse_send_mode(find_opt(args, "send-mode").value_or("sync"));

    // Inter-send delay per task. Mirrors src/bin/rtt.rs.
    const std::uint64_t delay_nanos = static_cast<std::uint64_t>(
        static_cast<double>(concurrency) / (load * 1'000'000.0) * 1'000'000'000.0);

    const std::uint64_t benchmark_start = monotonic_nanos() + 100'000'000ULL;
    const std::uint64_t deadline = benchmark_start + duration_ns;

    std::vector<std::thread> threads;
    std::vector<std::uint64_t> sent(concurrency, 0);
    std::vector<std::exception_ptr> failures(concurrency);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            Producer producer(c.url, c.transport, c.req_subject);
            std::vector<std::uint8_t> payload(c.item_size, 0);

            std::uint64_t next_send_ns =
                benchmark_start + (t * delay_nanos) / concurrency;

            busy_wait_until(benchmark_start);

            std::uint64_t count = 0;
            while (true) {
                busy_wait_until(next_send_ns);
                if (next_send_ns > deadline) break;

                std::uint64_t ts = monotonic_nanos();
                std::memcpy(payload.data(), &ts, 8);
                try {
                    if (send_mode == SendMode::sync_mode) {
                        producer.send_sync(payload.data(), payload.size());
                    } else {
                        producer.send_buffered(payload.data(), payload.size());
                    }
                    ++count;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "send error: %s\n", e.what());
                    std::this_thread::yield();
                    continue;
                }
                next_send_ns += delay_nanos;
            }
            if (send_mode == SendMode::async_mode) {
                // One settle per thread so nothing buffered is left unsent
                // on exit. A failed settle is fatal: `count` may include
                // messages that never reached the broker.
                try {
                    producer.flush_batch();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "flush error: %s\n", e.what());
                    failures[t] = std::current_exception();
                }
            }
            sent[t] = count;
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t benchmark_end = monotonic_nanos();
    rethrow_task_failure("sender", failures);
    std::uint64_t total = 0;
    for (auto n : sent) total += n;
    const double secs = (benchmark_end - benchmark_start) / 1e9;
    std::cout << "Sender " << id << ": sent " << total << " messages ("
              << (total / secs / 1e6) << " MOps/s)\n";
}

struct Measurement {
    std::uint64_t send_time_ns;
    std::uint64_t receive_time_ns;
};

void cmd_receive(const Common& c, const std::vector<std::string>& args) {
    auto id = parse_size(require_opt(args, "id", "i"));
    auto concurrency = parse_size(require_opt(args, "concurrency", "c"));
    auto duration_ns = parse_duration_ns(require_opt(args, "duration", "d"));
    auto output = find_opt(args, "output", "o");

    const std::uint64_t benchmark_start = monotonic_nanos();
    const std::uint64_t deadline = benchmark_start + duration_ns;

    std::vector<std::thread> threads;
    std::vector<std::vector<Measurement>> per_thread(concurrency);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            Consumer consumer(c.url, c.transport, c.resp_subject);
            auto& out = per_thread[t];
            out.reserve(1 << 16);
            std::vector<std::uint8_t> buf;

            while (true) {
                if (monotonic_nanos() > deadline) break;
                try {
                    if (!consumer.recv(buf)) continue;  // 0.5s timeout
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "recv error: %s\n", e.what());
                    continue;
                }
                const std::uint64_t recv_time = monotonic_nanos();
                if (buf.size() >= 8) {
                    std::uint64_t send_time;
                    std::memcpy(&send_time, buf.data(), 8);
                    out.push_back({send_time, recv_time});
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t benchmark_end = monotonic_nanos();
    std::vector<Measurement> all;
    std::size_t total = 0;
    for (auto& v : per_thread) total += v.size();
    all.reserve(total);
    for (auto& v : per_thread) all.insert(all.end(), v.begin(), v.end());

    const double secs = (benchmark_end - benchmark_start) / 1e9;
    std::cout << "Receiver " << id << ": received " << all.size() << " messages ("
              << (all.size() / secs / 1e6) << " MOps/s)\n";

    if (!all.empty()) {
        std::vector<std::uint64_t> rtts;
        rtts.reserve(all.size());
        for (const auto& m : all) {
            rtts.push_back(m.receive_time_ns >= m.send_time_ns
                               ? m.receive_time_ns - m.send_time_ns
                               : 0);
        }
        std::sort(rtts.begin(), rtts.end());
        const std::size_t n = rtts.size();
        std::uint64_t sum = 0;
        for (auto r : rtts) sum += r;
        std::cout << "RTT Statistics (" << n << " samples):\n"
                  << "  Min:  " << rtts.front() << " ns\n"
                  << "  Avg:  " << (sum / n) << " ns\n"
                  << "  P50:  " << rtts[n / 2] << " ns\n"
                  << "  P99:  " << rtts[(n * 99) / 100] << " ns\n"
                  << "  P999: " << rtts[std::min(n - 1, (n * 999) / 1000)] << " ns\n"
                  << "  Max:  " << rtts.back() << " ns\n";
    }

    if (output) {
        std::ofstream ofs(*output);
        if (!ofs) throw std::runtime_error("could not open " + *output);
        ofs << "send_time_ns,receive_time_ns\n";
        for (const auto& m : all) {
            ofs << m.send_time_ns << ',' << m.receive_time_ns << '\n';
        }
        std::cout << "Wrote " << all.size() << " measurements to " << *output << '\n';
    }
}

void cmd_server(const Common& c, const std::vector<std::string>& args) {
    auto id = parse_size(require_opt(args, "id", "i"));
    auto concurrency = parse_size(require_opt(args, "concurrency", "c"));
    auto duration_ns = parse_duration_ns(require_opt(args, "duration", "d"));
    // Validated before any broker contact so a bad value fails fast.
    auto send_mode = parse_send_mode(find_opt(args, "send-mode").value_or("sync"));

    const std::uint64_t benchmark_start = monotonic_nanos();
    const std::uint64_t deadline = benchmark_start + duration_ns;

    // Each server thread owns two clients: one consuming the request subject
    // and one publishing to the response subject. NATS handles are not
    // shared across threads.
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> echoed(concurrency, 0);
    std::vector<std::exception_ptr> failures(concurrency);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            Consumer consumer(c.url, c.transport, c.req_subject);
            Producer producer(c.url, c.transport, c.resp_subject);

            std::vector<std::uint8_t> buf;
            std::uint64_t count = 0;
            while (true) {
                if (monotonic_nanos() > deadline) break;
                try {
                    // Spec ordering: fetch request, republish to the response
                    // subject, THEN ack. A crash between the two leaves the
                    // request unacked, so the js WorkQueue redelivers it
                    // (at-least-once). Ack is a no-op for core.
                    AckHandle ack;
                    if (!consumer.recv(buf, ack)) continue;
                    if (send_mode == SendMode::sync_mode) {
                        producer.send_sync(buf.data(), buf.size());
                    } else {
                        // The response is only confirmed by the end-of-run
                        // settle, so the ack below precedes broker
                        // confirmation: a crash before the settle can lose
                        // the response without redelivery (weaker than the
                        // sync at-least-once chain).
                        producer.send_buffered(buf.data(), buf.size());
                    }
                    ack.ack();
                    ++count;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "server error: %s\n", e.what());
                    continue;
                }
            }
            if (send_mode == SendMode::async_mode) {
                // One settle per thread so no buffered response is lost on
                // exit; a failed settle is fatal (see cmd_send).
                try {
                    producer.flush_batch();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "flush error: %s\n", e.what());
                    failures[t] = std::current_exception();
                }
            }
            echoed[t] = count;
        });
    }
    for (auto& th : threads) th.join();

    const std::uint64_t benchmark_end = monotonic_nanos();
    rethrow_task_failure("server", failures);
    std::uint64_t total = 0;
    for (auto n : echoed) total += n;
    const double secs = (benchmark_end - benchmark_start) / 1e9;
    std::cout << "Server " << id << ": echoed " << total << " messages ("
              << (total / secs / 1e6) << " MOps/s)\n";
}

void print_usage() {
    std::cerr <<
        "rtt --url URL --transport core|js -s SIZE [--req-subject S] [--resp-subject S] SUBCOMMAND\n"
        "Subcommands:\n"
        "  init\n"
        "  send    --id N --concurrency N --duration D --load Mops [--send-mode sync|async]\n"
        "  server  --id N --concurrency N --duration D [--send-mode sync|async]\n"
        "  receive --id N --concurrency N --duration D [--output PATH]\n"
        "Transports: core (plain pub/sub + queue group), js (JetStream WorkQueue)\n";
}

}  // namespace

int main(int argc, char** argv) try {
    auto args = argv_to_vec(argc, argv);
    if (args.size() < 2) {
        print_usage();
        return 2;
    }
    // Find the subcommand: the first non-flag positional after the program.
    std::string subcmd;
    std::size_t subcmd_idx = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a.empty() || a[0] == '-') {
            // skip "--key value" pairs we don't care about for splitting
            continue;
        }
        // Previous arg might be a flag expecting a value
        if (i > 0 && !args[i - 1].empty() && args[i - 1][0] == '-' &&
            args[i - 1].find('=') == std::string::npos) {
            continue;
        }
        subcmd = a;
        subcmd_idx = i;
        break;
    }
    if (subcmd.empty()) {
        print_usage();
        return 2;
    }

    // Everything except args[0] and args[subcmd_idx] is fair game for the
    // common+subcommand option parser.
    std::vector<std::string> rest;
    rest.reserve(args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (i == subcmd_idx) continue;
        rest.push_back(args[i]);
    }

    Common c = parse_common(rest);
    if (subcmd == "init") {
        cmd_init(c);
    } else if (subcmd == "send") {
        cmd_send(c, rest);
    } else if (subcmd == "receive") {
        cmd_receive(c, rest);
    } else if (subcmd == "server") {
        cmd_server(c, rest);
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
