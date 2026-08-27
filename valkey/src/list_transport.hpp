// Queue operations over Valkey.
//
// Invariants:
// - Each Producer/Consumer owns one valkeyContext* and is not thread-safe.
// - BLPOP timeout is 0.5 seconds so deadline checks can run periodically.
// - Payloads are opaque bytes. The RTT binary writes a CLOCK_MONOTONIC_RAW
//   timestamp into the first 8 bytes; this layer never inspects them.
// - Pipelined sends (send_buffered) leave their replies unread on the
//   connection; the owner settles them with flush_batch(). Nothing drains
//   them implicitly, not even the destructor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "connection.hpp"

namespace vrdma {

// Upper bound on replies left unread by send_buffered() before one is
// consumed. Matches Pulsar's maxPendingMessages default so the async modes
// of the harnesses bound their in-flight window alike.
inline constexpr std::size_t kMaxPipelined = 1000;

class ListProducer {
   public:
    ListProducer(ContextPtr ctx, std::string key);

    // RPUSH key payload and wait for the reply. Throws on transport error.
    void send(const std::uint8_t* data, std::size_t len);

    // Pipelined RPUSH: the command is written to the socket before returning
    // but its reply is left for later. Once kMaxPipelined replies are
    // pending, one is read first so the window stays bounded. Pair with
    // flush_batch(). Throws on transport error.
    void send_buffered(const std::uint8_t* data, std::size_t len);

    // Queue an RPUSH without writing it: the whole batch leaves in one
    // write from flush_batch(). libvalkey's RDMA transport posts one work
    // request per write and has a 1024-entry send queue whose completions
    // are only signalled every 1024th post, so back-to-back per-command
    // writes fail after 1024 commands (ibv_post_send ENOMEM); batching the
    // writes keeps the queue depth at one request per batch. Used by the
    // throughput producer; rtt keeps send_buffered() so each message leaves
    // immediately (its sends are paced, which avoids the limit).
    void enqueue(const std::uint8_t* data, std::size_t len);

    // Read every pending reply. Throws on transport error or an error reply;
    // callers treat that as fatal (delivery of the pipelined items cannot be
    // claimed).
    void flush_batch();

   private:
    void read_reply();

    ContextPtr ctx_;
    std::string key_;
    std::size_t pending_ = 0;  // replies not yet read
};

class ListConsumer {
   public:
    ListConsumer(ContextPtr ctx, std::string key);

    // BLPOP key 0.5
    // On success, returns true and appends the payload bytes to `out`.
    // On timeout, returns false (out is left untouched).
    // On transport error, throws.
    bool recv(std::vector<std::uint8_t>& out);

   private:
    ContextPtr ctx_;
    std::string key_;
};

// DEL key — resets list state on the broker.
void init_list_key(valkeyContext* ctx, const std::string& key);

}  // namespace vrdma
