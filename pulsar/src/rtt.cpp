// Round-trip-time benchmark using C++ and pulsar-client-cpp.
//
// Methodology (mirrors valkey/src/rtt.cpp and shadix/src/bin/rtt.rs):
//
// - Three roles connected through a dedicated Pulsar broker:
//     `send`    publishes timestamped requests to the request topic
//     `server`  consumes requests (Shared subscription "bench") and
//               re-publishes the payload verbatim to the response topic
//     `receive` consumes responses, reads the embedded timestamp,
//               and writes (send_time_ns, receive_time_ns) rows to CSV.
// - Sender and receiver must run on the same host (RTT is the difference
//   of two CLOCK_MONOTONIC_RAW reads).
// - item_size must be >= 8 (first 8 bytes carry the timestamp).
// - One pulsar::Client per worker thread (ioThreads=1, stats disabled).
// - `send` and `server` take --send-mode sync|async (default sync):
//     sync   batching disabled, one blocking send per message, so client-side
//            buffering does not distort the latency measurement.
//     async  the client's batching producer (defaults: 1000 msgs / 128 KB /
//            10 ms linger), sendAsync per message, and ONE flush() per thread
//            after the duration loop so nothing is left unsent on exit. The
//            measured RTT then includes the batching linger on each hop. A
//            failed flush is fatal: the process exits 1 without a summary.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "args.hpp"
#include "monotonic.hpp"
#include "pulsar_transport.hpp"

using namespace pulsarbench;

namespace {

struct Common {
    std::string url;
    std::string admin_url;
    std::string transport;  // "persistent" | "non-persistent"
    std::size_t item_size;
    std::string req_topic;
    std::string resp_topic;
};

Common parse_common(const std::vector<std::string>& args) {
    Common c;
    c.url = find_opt(args, "url").value_or("pulsar://127.0.0.1:6650");
    c.admin_url = find_opt(args, "admin-url").value_or("http://127.0.0.1:8080");
    c.transport = require_opt(args, "transport", "t");
    validate_transport(c.transport);
    c.item_size = parse_size(require_opt(args, "item-size", "s"));
    if (c.item_size < 8) {
        throw std::runtime_error(
            "item-size must be >= 8 (first 8 bytes carry the timestamp)");
    }
    c.req_topic = find_opt(args, "req-topic").value_or("rb-rtt-req");
    c.resp_topic = find_opt(args, "resp-topic").value_or("rb-rtt-resp");
    return c;
}

void cmd_init(const Common& c) {
    ClientHandle handle(c.url);
    init_topic(handle.client(), c.admin_url, c.transport, c.req_topic);
    init_topic(handle.client(), c.admin_url, c.transport, c.resp_topic);
    std::cout << "Initialized benchmark topics: req=" << c.req_topic
              << " resp=" << c.resp_topic << '\n';
}

void busy_wait_until(std::uint64_t target_ns) {
    while (monotonic_nanos() < target_ns) {
        std::this_thread::yield();
    }
}

// Any task failure aborts the run (exit 1) before the summary is printed:
// a partial run must never look like a complete one.
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
    // Validated here, before any client is created.
    const SendMode mode =
        parse_send_mode(find_opt(args, "send-mode").value_or("sync"));

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
            try {
                ClientHandle ctx(c.url);
                TopicProducer producer(ctx.client(), c.transport, c.req_topic,
                                       mode);
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
                        producer.send(payload.data(), payload.size());
                        ++count;
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "send error: %s\n", e.what());
                        std::this_thread::yield();
                        continue;
                    }
                    next_send_ns += delay_nanos;
                }
                sent[t] = count;
                if (mode == SendMode::Async) {
                    // Settle: every buffered request must reach the broker
                    // before the thread exits. A failed settle means some
                    // never did, so it is fatal.
                    producer.flush();
                }
            } catch (...) {
                failures[t] = std::current_exception();
            }
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
            ClientHandle ctx(c.url);
            TopicConsumer consumer(ctx.client(), c.transport, c.resp_topic);
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
    // Validated here, before any client is created.
    const SendMode mode =
        parse_send_mode(find_opt(args, "send-mode").value_or("sync"));

    const std::uint64_t benchmark_start = monotonic_nanos();
    const std::uint64_t deadline = benchmark_start + duration_ns;

    // Each server thread owns one client with a consumer on the request
    // topic and a producer on the response topic. Clients are not shared
    // across threads.
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> echoed(concurrency, 0);
    std::vector<std::exception_ptr> failures(concurrency);
    threads.reserve(concurrency);

    for (std::size_t t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            try {
                ClientHandle ctx(c.url);
                TopicConsumer consumer(ctx.client(), c.transport, c.req_topic);
                TopicProducer producer(ctx.client(), c.transport, c.resp_topic,
                                       mode);

                std::vector<std::uint8_t> buf;
                std::uint64_t count = 0;
                while (true) {
                    if (monotonic_nanos() > deadline) break;
                    try {
                        if (!consumer.recv(buf)) continue;
                        producer.send(buf.data(), buf.size());
                        ++count;
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "server error: %s\n", e.what());
                        continue;
                    }
                }
                echoed[t] = count;
                if (mode == SendMode::Async) {
                    // Settle: every buffered echo must reach the broker
                    // before the thread exits. A failed settle means some
                    // never did, so it is fatal.
                    producer.flush();
                }
            } catch (...) {
                failures[t] = std::current_exception();
            }
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
        "rtt --url URL --transport {persistent|non-persistent} -s SIZE\n"
        "    [--req-topic T] [--resp-topic T] [--admin-url URL] SUBCOMMAND\n"
        "Subcommands:\n"
        "  init\n"
        "  send    --id N --concurrency N --duration D --load Mops\n"
        "          [--send-mode sync|async]\n"
        "  server  --id N --concurrency N --duration D [--send-mode sync|async]\n"
        "  receive --id N --concurrency N --duration D [--output PATH]\n"
        "Topic names are logical; the transport prefixes them as\n"
        "{persistent|non-persistent}://public/default/<name>.\n";
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
