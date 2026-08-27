// Queue operations over Apache Pulsar.
//
// Invariants:
// - One pulsar::Client per worker thread (ioThreads=1, stats disabled,
//   memory limit 0 = unlimited). Client handles are not shared across
//   threads.
// - Consumers use a Shared subscription named "bench" with a 0.5s receive
//   timeout so deadline checks can run periodically; every received message
//   is acknowledged (fire-and-forget) for destructive queue semantics.
// - Payloads are opaque bytes. The RTT binary writes a CLOCK_MONOTONIC_RAW
//   timestamp into the first 8 bytes; this layer never inspects them.
// - Topic names are logical; full_topic() prefixes them as
//   {persistent|non-persistent}://public/default/<name>.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulsar/Client.h>

namespace pulsarbench {

inline constexpr const char* kSubscription = "bench";

// Throws (with the accepted values in the message) unless `transport` is
// "persistent" or "non-persistent".
void validate_transport(const std::string& transport);

// {persistent|non-persistent}://public/default/<name>
std::string full_topic(const std::string& transport, const std::string& name);

// One client per worker thread. ioThreads=1 keeps one IO thread per worker
// (core pinning via taskset applies to it); stats collection is disabled;
// memory limit is set to 0 (unlimited) so async pending messages are bounded
// by maxPendingMessages alone.
class ClientHandle {
   public:
    explicit ClientHandle(const std::string& url);
    ~ClientHandle();
    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;

    pulsar::Client& client() { return client_; }

   private:
    pulsar::Client client_;
};

// sync:  batching disabled, blocking Producer::send per item (waits for the
//        broker ack — the direct RPUSH analog).
// async: batching enabled (client defaults: max 1000 msgs / 128 KB / 10 ms),
//        blockIfQueueFull, Producer::sendAsync per item, flush() barrier
//        (throughput: at batch boundaries; rtt: once per thread after the
//        duration loop).
enum class SendMode { Sync, Async };

// Throws (with the accepted values in the message) unless `mode` is "sync"
// or "async".
SendMode parse_send_mode(const std::string& mode);

class TopicProducer {
   public:
    TopicProducer(pulsar::Client& client, const std::string& transport,
                  const std::string& topic, SendMode mode);
    ~TopicProducer();

    // Sync mode: blocking send; throws on error.
    // Async mode: buffered sendAsync (blocks only when the pending queue is
    // full); failures surface at the next flush().
    void send(const std::uint8_t* data, std::size_t len);

    // Barrier: wait until all outstanding sends are acked. Throws if any
    // asynchronous send failed since the last flush.
    void flush();

   private:
    pulsar::Producer producer_;
    SendMode mode_;
    std::shared_ptr<std::atomic<std::uint64_t>> async_errors_;
};

class TopicConsumer {
   public:
    TopicConsumer(pulsar::Client& client, const std::string& transport,
                  const std::string& topic);
    ~TopicConsumer();

    // Receive with a 0.5s timeout.
    // On success, acknowledges the message (async, fire-and-forget), returns
    // true, and replaces `out` with the payload bytes.
    // On timeout, returns false (out is left untouched).
    // On transport error, throws.
    bool recv(std::vector<std::uint8_t>& out);

   private:
    pulsar::Consumer consumer_;
};

// Reset broker state for one logical topic.
// - persistent: via the admin REST API (libcurl):
//   DELETE /admin/v2/persistent/public/default/<topic>?force=true (a 404 for
//   a missing topic is ignored), then
//   PUT /admin/v2/persistent/public/default/<topic>/subscription/bench with
//   JSON body "earliest". This drops any stale backlog and re-creates the
//   topic with the subscription already attached, so no messages are lost
//   before consumers attach.
// - non-persistent: connectivity check only (create + close a producer); no
//   broker state survives, and messages published without a connected
//   subscription are dropped, so consumers must start first.
void init_topic(pulsar::Client& client, const std::string& admin_url,
                const std::string& transport, const std::string& topic);

}  // namespace pulsarbench
