#include "connection.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

#ifdef VRDMA_HAVE_RDMA
#include <valkey/rdma.h>
#endif

namespace vrdma {
namespace {

// "scheme://" prefix scan; returns scheme + advances `rest` past "://".
Scheme parse_scheme(std::string_view in, std::string_view& rest) {
    auto sep = in.find("://");
    if (sep == std::string_view::npos) {
        throw std::invalid_argument(std::string("url missing scheme: ") +
                                    std::string(in));
    }
    auto scheme = in.substr(0, sep);
    rest = in.substr(sep + 3);
    if (scheme == "rdma" || scheme == "valkey+rdma") return Scheme::Rdma;
    if (scheme == "tcp" || scheme == "redis" || scheme == "valkey") {
        return Scheme::Tcp;
    }
    throw std::invalid_argument(std::string("unknown url scheme: ") +
                                std::string(scheme));
}

void ensure_rdma_initialized() {
#ifdef VRDMA_HAVE_RDMA
    static std::once_flag flag;
    static int init_result = 0;
    std::call_once(flag, [] {
        // libvalkey: registers the RDMA connector. With ENABLE_DLOPEN_RDMA
        // the call also dlopens librdmacm/libibverbs; failure here means
        // those libs are missing on the host.
        init_result = valkeyInitiateRdma();
    });
    if (init_result != 0) {
        throw std::runtime_error(
            "valkeyInitiateRdma() failed — is librdmacm/libibverbs installed?");
    }
#else
    throw std::runtime_error(
        "this binary was built without RDMA support "
        "(reconfigure with -DBUILD_RDMA=ON on a host with rdma-core)");
#endif
}

void ensure_sigpipe_ignored() {
    static std::once_flag flag;
    std::call_once(flag, [] { std::signal(SIGPIPE, SIG_IGN); });
}

}  // namespace

ParsedUrl parse_url(const std::string& url) {
    std::string_view rest;
    Scheme scheme = parse_scheme(url, rest);

    // Strip optional query string for source addr.
    std::string source_addr;
    auto qmark = rest.find('?');
    if (qmark != std::string_view::npos) {
        auto query = rest.substr(qmark + 1);
        rest = rest.substr(0, qmark);
        // single-key query: "src=10.x.y.z"
        const std::string_view key = "src=";
        if (query.substr(0, key.size()) == key) {
            source_addr.assign(query.substr(key.size()));
        }
    }

    auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument(std::string("url missing port: ") + url);
    }
    std::string host(rest.substr(0, colon));
    int port = 0;
    try {
        port = std::stoi(std::string(rest.substr(colon + 1)));
    } catch (...) {
        throw std::invalid_argument(std::string("url has invalid port: ") + url);
    }
    return ParsedUrl{scheme, std::move(host), port, std::move(source_addr)};
}

ContextPtr make_connection(const ParsedUrl& url) {
    ensure_sigpipe_ignored();
    valkeyOptions opt{};
    if (url.scheme == Scheme::Rdma) {
        ensure_rdma_initialized();
#ifdef VRDMA_HAVE_RDMA
        if (url.source_addr.empty()) {
            VALKEY_OPTIONS_SET_RDMA(&opt, url.host.c_str(), url.port);
        } else {
            VALKEY_OPTIONS_SET_RDMA_WITH_SOURCE_ADDR(
                &opt, url.host.c_str(), url.port, url.source_addr.c_str());
        }
#endif
    } else {
        VALKEY_OPTIONS_SET_TCP(&opt, url.host.c_str(), url.port);
    }

    ContextPtr ctx(valkeyConnectWithOptions(&opt));
    if (!ctx) {
        throw ConnectionError("valkeyConnectWithOptions returned NULL");
    }
    if (ctx->err) {
        std::string msg = ctx->errstr;
        throw ConnectionError("connection failed: " + msg);
    }
    // libvalkey 0.4.0 returns an RDMA context with err == 0 but no private
    // context / CONNECTED flag when the peer is unreachable; the first write
    // would then segfault inside valkeyRdmaWrite. Refuse it here.
    if (!(ctx->flags & VALKEY_CONNECTED)) {
        throw ConnectionError("connection failed: not connected (" +
                              std::string(url.scheme == Scheme::Rdma ? "rdma" : "tcp") +
                              " peer unreachable?)");
    }
    if (const char* d = std::getenv("VRDMA_DEBUG_CONNECT")) {
        (void)d;
        std::fprintf(stderr,
                     "[vrdma] connect ok scheme=%s fd=%d privctx=%p flags=0x%x err=%d\n",
                     url.scheme == Scheme::Rdma ? "rdma" : "tcp",
                     ctx->fd, (void*)ctx->privctx, ctx->flags, ctx->err);
    }
    return ctx;
}

}  // namespace vrdma
