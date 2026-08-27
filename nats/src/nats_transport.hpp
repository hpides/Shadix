// Queue operations over NATS (core pub/sub and JetStream).
//
// Invariants:
// - Each Producer/Consumer owns one natsConnection (plus jsCtx for the `js`
//   transport) and is not thread-safe; use one per thread.
// - Receive timeout is 0.5 seconds so deadline checks can run periodically.
// - Payloads are opaque bytes. The RTT binary writes a CLOCK_MONOTONIC_RAW
//   timestamp into the first 8 bytes; this layer never inspects them.
// - `core`: plain publish + queue-group "bench" subscription. At-most-once:
//   no acks; the broker drops messages for slow consumers.
// - `js`: file-backed WorkQueue streams (stream name derived from the
//   subject) + durable pull consumer "bench" with explicit acks
//   (destructive read). `init` (re)creates streams and consumers so other
//   roles just bind.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <mutex>
#include <string>
#include <vector>

#include <nats.h>

namespace natsbench {

enum class Transport { core, js };

inline Transport parse_transport(const std::string& s) {
    if (s == "core") return Transport::core;
    if (s == "js") return Transport::js;
    throw std::runtime_error("unknown transport `" + s +
                             "` (accepted values: core, js)");
}

inline const char* transport_name(Transport t) {
    return t == Transport::core ? "core" : "js";
}

// How a Producer settles its sends (shared by the rtt and throughput CLIs):
//   sync_mode:  send_sync per message (one broker round trip each)
//   async_mode: send_buffered per message, flush_batch() as the barrier
enum class SendMode { sync_mode, async_mode };

inline SendMode parse_send_mode(const std::string& s) {
    if (s == "sync") return SendMode::sync_mode;
    if (s == "async") return SendMode::async_mode;
    throw std::runtime_error("invalid --send-mode `" + s +
                             "` (accepted values: sync, async)");
}

// JetStream stream name derived from a subject: uppercase, dots -> underscores.
// rb.rtt.req -> RB_RTT_REQ
inline std::string stream_name_for(const std::string& subject) {
    std::string out;
    out.reserve(subject.size());
    for (char c : subject) {
        if (c == '.') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(
                std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// Queue-group (core) / durable consumer (js) name shared by all consumers.
inline constexpr const char* kBenchGroup = "bench";

// One NATS connection (+ JetStream context for `js`). Owned by exactly one
// Producer or Consumer; never shared across threads.
class Client {
   public:
    Client(const std::string& url, Transport transport);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    natsConnection* conn() { return nc_; }
    jsCtx* js() { return js_; }  // nullptr for the core transport
    Transport transport() const { return transport_; }

    // Negative PubAcks for js_PublishAsync are only reported through the
    // jsCtx error handler; this returns (and clears) the count collected
    // since the last call, plus the first error text. Zero means clean.
    std::uint64_t take_pub_errors(std::string& first_error);

   private:
    static void pub_ack_err_handler(jsCtx* js, jsPubAckErr* pae, void* closure);

    Transport transport_;
    natsConnection* nc_ = nullptr;
    jsCtx* js_ = nullptr;
    std::mutex pub_err_mu_;
    std::uint64_t pub_errors_ = 0;
    std::string first_pub_error_;
};

class Producer {
   public:
    Producer(const std::string& url, Transport transport, std::string subject);

    // Synchronous send: the call returns only after a broker round trip.
    //   core: Publish + FlushTimeout (PING/PONG round trip per message)
    //   js:   js_Publish (waits for the PubAck)
    // Throws on transport error.
    void send_sync(const std::uint8_t* data, std::size_t len);

    // Buffered send: hand the message to the client library and return.
    //   core: Publish (client-side buffer)
    //   js:   js_PublishAsync
    // Pair with flush_batch() at batch boundaries. Throws on transport error.
    void send_buffered(const std::uint8_t* data, std::size_t len);

    // Wait until all buffered sends reached the broker.
    //   core: FlushTimeout
    //   js:   js_PublishAsyncComplete (waits for all pending PubAcks,
    //         bounded by a 30s MaxWait so a dead broker fails the barrier
    //         instead of hanging it)
    // Throws on transport error or timeout; callers treat that as fatal
    // (batch completion cannot be claimed for undelivered items).
    void flush_batch();

   private:
    Client client_;
    std::string subject_;
};

// Deferred ack for one received message, filled in by
// Consumer::recv(out, ack). Lets a caller ack only after downstream work
// succeeded (the RTT echo server acks a request after republishing it, so a
// crash in between leaves the request unacked and the WorkQueue redelivers
// it — at-least-once). For the core transport the handle stays empty and
// ack() is a no-op. Destroying an unacked handle discards the message
// without acking it.
class AckHandle {
   public:
    AckHandle() = default;
    ~AckHandle() { reset(); }
    AckHandle(const AckHandle&) = delete;
    AckHandle& operator=(const AckHandle&) = delete;

    // js: fire-and-forget natsMsg_Ack (WorkQueue deletes the message).
    // No-op when empty (core transport, already acked, or never filled).
    void ack() {
        if (msg_ != nullptr) natsMsg_Ack(msg_, nullptr);
        reset();
    }

   private:
    friend class Consumer;
    void adopt(natsMsg* msg) {  // takes ownership; nullptr just clears
        reset();
        msg_ = msg;
    }
    void reset() {
        if (msg_ != nullptr) {
            natsMsg_Destroy(msg_);
            msg_ = nullptr;
        }
    }
    natsMsg* msg_ = nullptr;
};

class Consumer {
   public:
    Consumer(const std::string& url, Transport transport, std::string subject);
    ~Consumer();

    // 0.5s receive timeout.
    // On success, returns true and replaces `out` with the payload bytes
    // (js: the message is acked with a fire-and-forget natsMsg_Ack before
    // returning).
    // On timeout, returns false (out is left untouched).
    // On transport error, throws.
    bool recv(std::vector<std::uint8_t>& out);

    // Same as recv(out), but instead of acking before returning it fills
    // `ack` so the caller decides when (or whether) to ack. Core transport:
    // `ack` is left empty and ack() is a no-op.
    bool recv(std::vector<std::uint8_t>& out, AckHandle& ack);

   private:
    Client client_;
    std::string subject_;
    natsSubscription* sub_ = nullptr;
    // js only: buffered fetch results, handed out one recv() at a time.
    natsMsgList fetched_{};
    int next_msg_ = 0;
};

// Reset broker state for the given subjects.
//   core: connectivity check only (no broker state exists).
//   js:   delete + recreate the derived stream (file storage, WorkQueue
//         retention, 1 replica) and create the durable pull consumer
//         "bench" (explicit acks) for each subject.
void init_transport(const std::string& url, Transport transport,
                    const std::vector<std::string>& subjects);

}  // namespace natsbench
