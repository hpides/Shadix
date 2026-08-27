#include "list_transport.hpp"

#include <stdexcept>

namespace vrdma {
namespace {

struct ReplyDeleter {
    void operator()(void* r) const noexcept {
        if (r) freeReplyObject(r);
    }
};
using ReplyPtr = std::unique_ptr<valkeyReply, ReplyDeleter>;

ReplyPtr take_reply(valkeyReply* raw) {
    return ReplyPtr(raw);
}

[[noreturn]] void throw_ctx_error(valkeyContext* ctx, const char* op) {
    // valkeyContext::errstr is a fixed-size char array.
    throw ConnectionError(std::string(op) + ": " + ctx->errstr);
}

}  // namespace

ListProducer::ListProducer(ContextPtr ctx, std::string key)
    : ctx_(std::move(ctx)), key_(std::move(key)) {}

void ListProducer::send(const std::uint8_t* data, std::size_t len) {
    // argv form is binary-safe and avoids format-string surprises.
    const char* argv[3] = {"RPUSH", key_.c_str(), reinterpret_cast<const char*>(data)};
    const std::size_t argvlen[3] = {5, key_.size(), len};
    auto reply = take_reply(static_cast<valkeyReply*>(
        valkeyCommandArgv(ctx_.get(), 3, argv, argvlen)));
    if (!reply) {
        throw_ctx_error(ctx_.get(), "RPUSH");
    }
    if (reply->type == VALKEY_REPLY_ERROR) {
        throw std::runtime_error(std::string("RPUSH server error: ") + reply->str);
    }
}

void ListProducer::send_buffered(const std::uint8_t* data, std::size_t len) {
    const char* argv[3] = {"RPUSH", key_.c_str(), reinterpret_cast<const char*>(data)};
    const std::size_t argvlen[3] = {5, key_.size(), len};
    if (valkeyAppendCommandArgv(ctx_.get(), 3, argv, argvlen) != VALKEY_OK) {
        throw_ctx_error(ctx_.get(), "RPUSH append");
    }
    // Push the command onto the wire now; libvalkey would otherwise hold it
    // in the output buffer until the next reply read.
    int done = 0;
    while (!done) {
        if (valkeyBufferWrite(ctx_.get(), &done) != VALKEY_OK) {
            throw_ctx_error(ctx_.get(), "RPUSH write");
        }
    }
    ++pending_;
    if (pending_ >= kMaxPipelined) {
        read_reply();
    }
}

void ListProducer::enqueue(const std::uint8_t* data, std::size_t len) {
    const char* argv[3] = {"RPUSH", key_.c_str(), reinterpret_cast<const char*>(data)};
    const std::size_t argvlen[3] = {5, key_.size(), len};
    if (valkeyAppendCommandArgv(ctx_.get(), 3, argv, argvlen) != VALKEY_OK) {
        throw_ctx_error(ctx_.get(), "RPUSH append");
    }
    ++pending_;
}

void ListProducer::flush_batch() {
    // Write whatever is still queued (no-op after send_buffered), then
    // settle every outstanding reply.
    int done = 0;
    while (!done) {
        if (valkeyBufferWrite(ctx_.get(), &done) != VALKEY_OK) {
            throw_ctx_error(ctx_.get(), "RPUSH write");
        }
    }
    while (pending_ > 0) {
        read_reply();
    }
}

void ListProducer::read_reply() {
    void* raw = nullptr;
    const int rc = valkeyGetReply(ctx_.get(), &raw);
    auto reply = take_reply(static_cast<valkeyReply*>(raw));
    --pending_;
    if (rc != VALKEY_OK || !reply) {
        throw_ctx_error(ctx_.get(), "RPUSH");
    }
    if (reply->type == VALKEY_REPLY_ERROR) {
        // The command is already on the wire and the pipeline position is
        // lost, so the caller must not retry it: report it as fatal.
        throw ConnectionError(std::string("RPUSH server error (pipelined): ") +
                              reply->str);
    }
}

ListConsumer::ListConsumer(ContextPtr ctx, std::string key)
    : ctx_(std::move(ctx)), key_(std::move(key)) {}

bool ListConsumer::recv(std::vector<std::uint8_t>& out) {
    // BLPOP key 0.5
    const char* argv[3] = {"BLPOP", key_.c_str(), "0.5"};
    const std::size_t argvlen[3] = {5, key_.size(), 3};
    auto reply = take_reply(static_cast<valkeyReply*>(
        valkeyCommandArgv(ctx_.get(), 3, argv, argvlen)));
    if (!reply) {
        throw_ctx_error(ctx_.get(), "BLPOP");
    }
    if (reply->type == VALKEY_REPLY_NIL) {
        return false;  // timeout
    }
    if (reply->type != VALKEY_REPLY_ARRAY || reply->elements != 2) {
        throw std::runtime_error("BLPOP unexpected reply type");
    }
    valkeyReply* payload = reply->element[1];
    if (payload->type != VALKEY_REPLY_STRING) {
        throw std::runtime_error("BLPOP payload not a string");
    }
    out.assign(reinterpret_cast<std::uint8_t*>(payload->str),
               reinterpret_cast<std::uint8_t*>(payload->str) + payload->len);
    return true;
}

void init_list_key(valkeyContext* ctx, const std::string& key) {
    const char* argv[2] = {"DEL", key.c_str()};
    const std::size_t argvlen[2] = {3, key.size()};
    auto reply = take_reply(static_cast<valkeyReply*>(
        valkeyCommandArgv(ctx, 2, argv, argvlen)));
    if (!reply) {
        throw_ctx_error(ctx, "DEL");
    }
    if (reply->type == VALKEY_REPLY_ERROR) {
        throw std::runtime_error(std::string("DEL server error: ") + reply->str);
    }
}

}  // namespace vrdma
