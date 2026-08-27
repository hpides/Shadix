#include "nats_transport.hpp"

#include <cstring>
#include <iostream>

namespace natsbench {
namespace {

constexpr std::int64_t kRecvTimeoutMs = 500;   // deadline checks stay regular
constexpr std::int64_t kFlushTimeoutMs = 5000;
// Upper bound for the js async batch barrier (js_PublishAsyncComplete).
// Without it a dead broker would block the barrier forever; with it a broker
// failure surfaces as NATS_TIMEOUT, which flush_batch() turns into a fatal
// error. Generous (matches pulsar's 30s send timeout) so a healthy but
// heavily loaded broker does not trip it.
constexpr std::int64_t kBarrierTimeoutMs = 30000;
constexpr int kFetchBatch = 500;               // js pull fetch batch size
// nats.c only marshals max_ack_pending when it is > 0, so "unlimited" (-1)
// would silently fall back to the server default of 1000 and throttle the
// fetch pipeline to 1000 messages per fetch timeout. Use a cap far above any
// single run's message count instead.
constexpr std::int64_t kMaxAckPending = 10'000'000;

[[noreturn]] void throw_status(const char* op, natsStatus s) {
    throw std::runtime_error(std::string(op) + ": " + natsStatus_GetText(s));
}

void check(const char* op, natsStatus s) {
    if (s != NATS_OK) throw_status(op, s);
}

}  // namespace

// ----------------------------------------------------------------------------
// Client
// ----------------------------------------------------------------------------

Client::Client(const std::string& url, Transport transport)
    : transport_(transport) {
    check("connect", natsConnection_ConnectTo(&nc_, url.c_str()));
    if (transport_ == Transport::js) {
        // Without an error handler nats.c drops negative PubAcks of
        // js_PublishAsync silently and js_PublishAsyncComplete still
        // succeeds; the handler lets flush_batch() fail the barrier instead.
        jsOptions opts;
        natsStatus s = jsOptions_Init(&opts);
        if (s == NATS_OK) {
            opts.PublishAsync.ErrHandler = &Client::pub_ack_err_handler;
            opts.PublishAsync.ErrHandlerClosure = this;
            s = natsConnection_JetStream(&js_, nc_, &opts);
        }
        if (s != NATS_OK) {
            natsConnection_Destroy(nc_);
            nc_ = nullptr;
            throw_status("jetstream context", s);
        }
    }
}

Client::~Client() {
    if (js_ != nullptr) jsCtx_Destroy(js_);
    if (nc_ != nullptr) natsConnection_Destroy(nc_);
}

void Client::pub_ack_err_handler(jsCtx*, jsPubAckErr* pae, void* closure) {
    auto* self = static_cast<Client*>(closure);
    std::lock_guard<std::mutex> lock(self->pub_err_mu_);
    if (self->pub_errors_++ == 0) {
        self->first_pub_error_ =
            pae->ErrText != nullptr ? pae->ErrText : natsStatus_GetText(pae->Err);
    }
}

std::uint64_t Client::take_pub_errors(std::string& first_error) {
    std::lock_guard<std::mutex> lock(pub_err_mu_);
    const std::uint64_t n = pub_errors_;
    first_error = first_pub_error_;
    pub_errors_ = 0;
    first_pub_error_.clear();
    return n;
}

// ----------------------------------------------------------------------------
// Producer
// ----------------------------------------------------------------------------

Producer::Producer(const std::string& url, Transport transport,
                   std::string subject)
    : client_(url, transport), subject_(std::move(subject)) {}

void Producer::send_sync(const std::uint8_t* data, std::size_t len) {
    if (client_.transport() == Transport::core) {
        check("publish", natsConnection_Publish(client_.conn(), subject_.c_str(),
                                                data, static_cast<int>(len)));
        check("flush",
              natsConnection_FlushTimeout(client_.conn(), kFlushTimeoutMs));
    } else {
        jsErrCode jerr = static_cast<jsErrCode>(0);
        natsStatus s = js_Publish(nullptr, client_.js(), subject_.c_str(), data,
                                  static_cast<int>(len), nullptr, &jerr);
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("js publish: ") +
                                     natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }
    }
}

void Producer::send_buffered(const std::uint8_t* data, std::size_t len) {
    if (client_.transport() == Transport::core) {
        check("publish", natsConnection_Publish(client_.conn(), subject_.c_str(),
                                                data, static_cast<int>(len)));
    } else {
        check("js publish async",
              js_PublishAsync(client_.js(), subject_.c_str(), data,
                              static_cast<int>(len), nullptr));
    }
}

void Producer::flush_batch() {
    if (client_.transport() == Transport::core) {
        check("flush",
              natsConnection_FlushTimeout(client_.conn(), kFlushTimeoutMs));
    } else {
        // Bounded wait: js_PublishAsyncComplete with no options would wait
        // forever, so a dead broker would hang the barrier instead of
        // failing it.
        jsPubOptions opts;
        check("pub options", jsPubOptions_Init(&opts));
        opts.MaxWait = kBarrierTimeoutMs;
        check("js publish async complete",
              js_PublishAsyncComplete(client_.js(), &opts));
        std::string first_error;
        const std::uint64_t errors = client_.take_pub_errors(first_error);
        if (errors != 0) {
            throw std::runtime_error("js publish async: " +
                                     std::to_string(errors) +
                                     " PubAck error(s), first: " + first_error);
        }
    }
}

// ----------------------------------------------------------------------------
// Consumer
// ----------------------------------------------------------------------------

Consumer::Consumer(const std::string& url, Transport transport,
                   std::string subject)
    : client_(url, transport), subject_(std::move(subject)) {
    if (client_.transport() == Transport::core) {
        // Queue group -> destructive-read semantics across all consumers.
        check("queue subscribe",
              natsConnection_QueueSubscribeSync(&sub_, client_.conn(),
                                                subject_.c_str(), kBenchGroup));
        // No client-side drop under load (broker-side slow-consumer drops
        // are inherent to core NATS).
        check("set pending limits",
              natsSubscription_SetPendingLimits(sub_, -1, -1));
    } else {
        // Bind to the durable pull consumer created by `init`.
        jsSubOptions so;
        check("sub options", jsSubOptions_Init(&so));
        const std::string stream = stream_name_for(subject_);
        so.Stream = stream.c_str();
        jsErrCode jerr = static_cast<jsErrCode>(0);
        natsStatus s = js_PullSubscribe(&sub_, client_.js(), subject_.c_str(),
                                        kBenchGroup, nullptr, &so, &jerr);
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("js pull subscribe: ") +
                                     natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }
    }
}

Consumer::~Consumer() {
    natsMsgList_Destroy(&fetched_);  // destroys any not-yet-consumed messages
    if (sub_ != nullptr) natsSubscription_Destroy(sub_);
}

bool Consumer::recv(std::vector<std::uint8_t>& out) {
    AckHandle ack;
    if (!recv(out, ack)) return false;
    ack.ack();  // js: fire-and-forget; WorkQueue deletes on ack. core: no-op.
    return true;
}

bool Consumer::recv(std::vector<std::uint8_t>& out, AckHandle& ack) {
    if (client_.transport() == Transport::core) {
        natsMsg* msg = nullptr;
        natsStatus s = natsSubscription_NextMsg(&msg, sub_, kRecvTimeoutMs);
        if (s == NATS_TIMEOUT) return false;
        if (s != NATS_OK) throw_status("next msg", s);
        const auto* data =
            reinterpret_cast<const std::uint8_t*>(natsMsg_GetData(msg));
        out.assign(data, data + natsMsg_GetDataLength(msg));
        natsMsg_Destroy(msg);
        ack.adopt(nullptr);  // nothing to ack for core
        return true;
    }

    // js: hand out buffered fetch results one message at a time.
    if (next_msg_ >= fetched_.Count) {
        natsMsgList_Destroy(&fetched_);
        fetched_ = natsMsgList{};
        next_msg_ = 0;
        jsErrCode jerr = static_cast<jsErrCode>(0);
        natsStatus s = natsSubscription_Fetch(&fetched_, sub_, kFetchBatch,
                                              kRecvTimeoutMs, &jerr);
        if (s == NATS_TIMEOUT || fetched_.Count == 0) {
            natsMsgList_Destroy(&fetched_);
            fetched_ = natsMsgList{};
            return false;
        }
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("js fetch: ") +
                                     natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }
    }

    natsMsg* msg = fetched_.Msgs[next_msg_];
    fetched_.Msgs[next_msg_] = nullptr;  // the AckHandle owns it now
    ++next_msg_;
    const auto* data =
        reinterpret_cast<const std::uint8_t*>(natsMsg_GetData(msg));
    out.assign(data, data + natsMsg_GetDataLength(msg));
    ack.adopt(msg);
    return true;
}

// ----------------------------------------------------------------------------
// init
// ----------------------------------------------------------------------------

void init_transport(const std::string& url, Transport transport,
                    const std::vector<std::string>& subjects) {
    Client client(url, transport);  // connectivity check for both transports
    if (transport == Transport::core) return;  // no broker state to reset

    for (const auto& subject : subjects) {
        const std::string stream = stream_name_for(subject);
        jsErrCode jerr = static_cast<jsErrCode>(0);

        // Drop stale state (and its backlog) from previous runs.
        natsStatus s = js_DeleteStream(client.js(), stream.c_str(), nullptr,
                                       &jerr);
        if (s != NATS_OK && s != NATS_NOT_FOUND) {
            throw std::runtime_error(std::string("js delete stream ") + stream +
                                     ": " + natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }

        // File-backed WorkQueue stream: messages are deleted once acked
        // (destructive-read analog of a list/queue).
        jsStreamConfig cfg;
        check("stream config", jsStreamConfig_Init(&cfg));
        cfg.Name = stream.c_str();
        const char* subj_arr[1] = {subject.c_str()};
        cfg.Subjects = subj_arr;
        cfg.SubjectsLen = 1;
        cfg.Retention = js_WorkQueuePolicy;
        cfg.Storage = js_FileStorage;
        cfg.Replicas = 1;
        jerr = static_cast<jsErrCode>(0);
        s = js_AddStream(nullptr, client.js(), &cfg, nullptr, &jerr);
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("js add stream ") + stream +
                                     ": " + natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }

        // Durable pull consumer with explicit acks; consumers just bind.
        jsConsumerConfig cc;
        check("consumer config", jsConsumerConfig_Init(&cc));
        cc.Durable = kBenchGroup;
        cc.AckPolicy = js_AckExplicit;
        cc.MaxAckPending = kMaxAckPending;  // don't throttle the fetch pipeline
        jerr = static_cast<jsErrCode>(0);
        jsConsumerInfo* ci = nullptr;
        s = js_AddConsumer(&ci, client.js(), stream.c_str(), &cc, nullptr,
                           &jerr);
        if (s != NATS_OK) {
            throw std::runtime_error(std::string("js add consumer on ") +
                                     stream + ": " + natsStatus_GetText(s) +
                                     " (jerr=" + std::to_string(jerr) + ")");
        }
        const std::int64_t applied = ci->Config->MaxAckPending;
        jsConsumerInfo_Destroy(ci);
        if (applied != kMaxAckPending) {
            throw std::runtime_error(
                "js consumer on " + stream + ": server applied MaxAckPending=" +
                std::to_string(applied) + ", expected " +
                std::to_string(kMaxAckPending));
        }
        std::cout << "Created stream " << stream << " (file, WorkQueue) and consumer "
                  << kBenchGroup << " (MaxAckPending=" << applied << ")\n";
    }
}

}  // namespace natsbench
