// Per-thread blocking libvalkey connection, selecting TCP or RDMA based on
// the URL scheme.
//
// Accepted URLs:
//   tcp://host:port
//   redis://host:port
//   valkey://host:port
//   rdma://host:port[?src=<local-ip>]
//   valkey+rdma://host:port[?src=<local-ip>]
//
// The ?src=<ip> query is honored only for RDMA and binds the connection to a
// specific local IB device IP via VALKEY_OPTIONS_SET_RDMA_WITH_SOURCE_ADDR.
//
// Invariants:
// - Each Connection wraps exactly one valkeyContext*. BLPOP parks the
//   connection while waiting, so connections must not be shared across
//   threads.
// - valkeyInitiateRdma() is called exactly once per process the first time
//   an rdma:// URL is opened.
// - SIGPIPE is ignored process-wide the first time any connection is opened:
//   libvalkey writes with send(..., 0), so a broker that went away would
//   otherwise kill the process on the next write instead of surfacing as a
//   ConnectionError.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <valkey/valkey.h>

namespace vrdma {

// Transport-level failure of a valkeyContext (connect error or ctx->err set
// afterwards). A context never recovers from one — libvalkey does not
// reconnect — so callers must not retry on the same connection.
struct ConnectionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Scheme {
    Tcp,
    Rdma,
};

struct ParsedUrl {
    Scheme scheme;
    std::string host;
    int port;
    std::string source_addr;  // RDMA only; empty if unset
};

ParsedUrl parse_url(const std::string& url);

// Opens one blocking connection. Throws ConnectionError (a
// std::runtime_error) with the libvalkey errstr on failure.
struct ContextDeleter {
    void operator()(valkeyContext* c) const noexcept {
        if (c) valkeyFree(c);
    }
};
using ContextPtr = std::unique_ptr<valkeyContext, ContextDeleter>;

ContextPtr make_connection(const ParsedUrl& url);
inline ContextPtr make_connection(const std::string& url) {
    return make_connection(parse_url(url));
}

}  // namespace vrdma
