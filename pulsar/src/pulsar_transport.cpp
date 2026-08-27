#include "pulsar_transport.hpp"

#include <curl/curl.h>

#include <mutex>
#include <stdexcept>

namespace pulsarbench {
namespace {

[[noreturn]] void throw_result(const char* op, pulsar::Result res) {
    throw std::runtime_error(std::string(op) + ": " + pulsar::strResult(res));
}

pulsar::ClientConfiguration make_client_config() {
    pulsar::ClientConfiguration conf;
    conf.setIOThreads(1);
    conf.setStatsIntervalInSeconds(0);  // disable stats collection
    conf.setMemoryLimit(0);             // unlimited (client default as of 4.x)
    return conf;
}

// ---------------------------------------------------------------------------
// Minimal libcurl helper for the admin REST API.
// ---------------------------------------------------------------------------

std::size_t collect_body(char* ptr, std::size_t size, std::size_t nmemb,
                         void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

void curl_global_once() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Performs `method` on `url`; `body` (may be nullptr) is sent as
// application/json. Returns the HTTP status code; throws on transport-level
// failure. The response body is appended to `response`.
long http_request(const std::string& method, const std::string& url,
                  const char* body, std::string& response) {
    curl_global_once();
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    struct curl_slist* headers = nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(method + " " + url + ": " +
                                 curl_easy_strerror(rc));
    }
    return code;
}

}  // namespace

void validate_transport(const std::string& transport) {
    if (transport != "persistent" && transport != "non-persistent") {
        throw std::runtime_error(
            "unknown transport `" + transport +
            "` (accepted: persistent, non-persistent)");
    }
}

std::string full_topic(const std::string& transport, const std::string& name) {
    validate_transport(transport);
    return transport + "://public/default/" + name;
}

SendMode parse_send_mode(const std::string& mode) {
    if (mode == "sync") return SendMode::Sync;
    if (mode == "async") return SendMode::Async;
    throw std::runtime_error("unknown send-mode `" + mode +
                             "` (accepted: sync, async)");
}

// ---------------------------------------------------------------------------
// ClientHandle
// ---------------------------------------------------------------------------

ClientHandle::ClientHandle(const std::string& url)
    : client_(url, make_client_config()) {}

ClientHandle::~ClientHandle() {
    client_.close();  // best effort; ignore the result
}

// ---------------------------------------------------------------------------
// TopicProducer
// ---------------------------------------------------------------------------

TopicProducer::TopicProducer(pulsar::Client& client,
                             const std::string& transport,
                             const std::string& topic, SendMode mode)
    : mode_(mode),
      async_errors_(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    pulsar::ProducerConfiguration conf;
    if (mode_ == SendMode::Sync) {
        conf.setBatchingEnabled(false);
    } else {
        // Batching stays at the client defaults (enabled; up to 1000
        // messages / 128 KB / 10 ms per batch). Block the caller instead of
        // failing once maxPendingMessages (default 1000) is reached.
        conf.setBlockIfQueueFull(true);
    }
    // The (topic, conf, producer) overload is deprecated in pulsar-client-cpp
    // >= 4.2 in favor of createProducerV2, but remains the only spelling
    // available across the older releases a system-installed library might
    // be, so use it deliberately.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    pulsar::Result res =
        client.createProducer(full_topic(transport, topic), conf, producer_);
#pragma GCC diagnostic pop
    if (res != pulsar::ResultOk) throw_result("createProducer", res);
}

TopicProducer::~TopicProducer() {
    producer_.close();  // best effort; ignore the result
}

void TopicProducer::send(const std::uint8_t* data, std::size_t len) {
    pulsar::Message msg =
        pulsar::MessageBuilder().setContent(data, len).build();
    if (mode_ == SendMode::Sync) {
        pulsar::Result res = producer_.send(msg);
        if (res != pulsar::ResultOk) throw_result("send", res);
    } else {
        auto errors = async_errors_;
        producer_.sendAsync(msg, [errors](pulsar::Result res,
                                          const pulsar::MessageId&) {
            if (res != pulsar::ResultOk) errors->fetch_add(1);
        });
    }
}

void TopicProducer::flush() {
    pulsar::Result res = producer_.flush();
    if (res != pulsar::ResultOk) throw_result("flush", res);
    const auto errors = async_errors_->exchange(0);
    if (errors != 0) {
        throw std::runtime_error("flush: " + std::to_string(errors) +
                                 " async send(s) failed");
    }
}

// ---------------------------------------------------------------------------
// TopicConsumer
// ---------------------------------------------------------------------------

TopicConsumer::TopicConsumer(pulsar::Client& client,
                             const std::string& transport,
                             const std::string& topic) {
    pulsar::ConsumerConfiguration conf;
    conf.setConsumerType(pulsar::ConsumerShared);
    pulsar::Result res = client.subscribe(full_topic(transport, topic),
                                          kSubscription, conf, consumer_);
    if (res != pulsar::ResultOk) throw_result("subscribe", res);
}

TopicConsumer::~TopicConsumer() {
    consumer_.close();  // best effort; ignore the result
}

bool TopicConsumer::recv(std::vector<std::uint8_t>& out) {
    pulsar::Message msg;
    pulsar::Result res = consumer_.receive(msg, 500);
    if (res == pulsar::ResultTimeout) return false;
    if (res != pulsar::ResultOk) throw_result("receive", res);
    consumer_.acknowledgeAsync(msg, [](pulsar::Result) {});
    const auto* data = static_cast<const std::uint8_t*>(msg.getData());
    out.assign(data, data + msg.getLength());
    return true;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

void init_topic(pulsar::Client& client, const std::string& admin_url,
                const std::string& transport, const std::string& topic) {
    validate_transport(transport);
    if (transport == "persistent") {
        const std::string base =
            admin_url + "/admin/v2/persistent/public/default/" + topic;

        // Drop the topic (and any stale backlog / subscriptions). A 404
        // simply means it did not exist yet.
        std::string body;
        long code = http_request("DELETE", base + "?force=true", nullptr, body);
        if (code != 404 && (code < 200 || code >= 300)) {
            throw std::runtime_error("DELETE " + base + " failed: HTTP " +
                                     std::to_string(code) + " " + body);
        }

        // Re-create the topic with the shared subscription already attached
        // (cursor at earliest) so no message published before the consumers
        // attach is lost. Verified against Pulsar 4.x standalone: the body is
        // the JSON string "earliest" and success is HTTP 204.
        body.clear();
        code = http_request("PUT",
                            base + "/subscription/" + kSubscription,
                            "\"earliest\"", body);
        if (code < 200 || code >= 300) {
            throw std::runtime_error("PUT " + base + "/subscription/" +
                                     kSubscription + " failed: HTTP " +
                                     std::to_string(code) + " " + body);
        }
    } else {
        // Non-persistent topics hold no broker state to reset; just verify
        // that the broker is reachable and the topic can be attached to.
        pulsar::Producer probe;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        pulsar::Result res = client.createProducer(
            full_topic(transport, topic), pulsar::ProducerConfiguration(),
            probe);
#pragma GCC diagnostic pop
        if (res != pulsar::ResultOk) throw_result("connectivity check", res);
        probe.close();
    }
}

}  // namespace pulsarbench
