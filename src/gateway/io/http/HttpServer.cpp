#include "gateway/io/http/HttpServer.h"
#include "gateway/core/log/Logger.h"
#include "gateway/io/http/WebAsset.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace gateway {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using Error = boost::system::error_code;

constexpr std::size_t kHeaderLimit = 16 * 1024;
constexpr std::size_t kBodyLimit = 64 * 1024;
constexpr std::size_t kBufferLimit = kHeaderLimit + kBodyLimit + 16 * 1024;
constexpr std::size_t kConnectionLimit = 256;

/**
 * 业务线程只能持有此桥的 weak_ptr。短锁覆盖检查和 post，使停机失效与投递互斥，
 * 从而不会把任务提交给已析构的 io_context；session 也只在 HTTP 线程锁定/访问。
 */
class ReplyBridge {
public:
    explicit ReplyBridge(net::io_context& io) : io_(&io) {}
    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (io_) net::post(*io_, std::move(fn));
    }
    void disable() {
        std::lock_guard<std::mutex> lock(mutex_);
        io_ = nullptr;
    }
private:
    std::mutex mutex_;
    net::io_context* io_;
};

class Session;
class Server : public std::enable_shared_from_this<Server> {
public:
    Server(net::io_context& io, HttpRuntimeConfigProvider config,
           HttpRequestHandler handler, std::function<bool()> should_stop)
        : acceptor_(io), stop_timer_(io), accept_retry_timer_(io),
          bridge_(std::make_shared<ReplyBridge>(io)),
          config_(std::move(config)), handler_(std::move(handler)),
          should_stop_(std::move(should_stop)) {}
    ~Server() { bridge_->disable(); }

    unsigned short listen(int port) {
        acceptor_.open(tcp::v4());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind({tcp::v4(), static_cast<unsigned short>(port)});
        acceptor_.listen(net::socket_base::max_listen_connections);
        accept();
        checkStop();
        return acceptor_.local_endpoint().port();
    }
    std::chrono::seconds timeout() const {
        return std::chrono::seconds(std::max(1, config_ ? config_().idle_timeout_s : 5));
    }
    void remove(const std::shared_ptr<Session>& session) { sessions_.erase(session); }
    void stop();

    std::shared_ptr<ReplyBridge> bridge() const { return bridge_; }
    const HttpRequestHandler& handler() const { return handler_; }
private:
    void accept();
    void checkStop() {
        stop_timer_.expires_after(std::chrono::milliseconds(100));
        stop_timer_.async_wait([self = shared_from_this()](Error ec) {
            if (ec || self->stopping_) return;
            if (self->should_stop_ && self->should_stop_()) self->stop();
            else self->checkStop();
        });
    }

    tcp::acceptor acceptor_;
    net::steady_timer stop_timer_;
    net::steady_timer accept_retry_timer_;
    std::shared_ptr<ReplyBridge> bridge_;
    HttpRuntimeConfigProvider config_;
    HttpRequestHandler handler_;
    std::function<bool()> should_stop_;
    std::unordered_set<std::shared_ptr<Session>> sessions_;
    bool stopping_ = false;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<Server> server)
        : stream_(std::move(socket)), business_timer_(stream_.get_executor()),
          buffer_(kBufferLimit), server_(server) {}

    void read() {
        auto server = server_.lock();
        if (!server || closed_) return;
        version_ = 11;
        head_ = false;
        parser_.emplace();
        parser_->header_limit(static_cast<std::uint32_t>(kHeaderLimit));
        parser_->body_limit(kBodyLimit);
        stream_.expires_after(server->timeout());
        http::async_read(stream_, buffer_, *parser_,
            [self = shared_from_this()](Error ec, std::size_t) { self->onRead(ec); });
    }
    void close() {
        if (closed_) return;
        closed_ = true;
        waiting_ = false;
        Error ignored;
        business_timer_.cancel();
        stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream_.socket().close(ignored);
        if (auto server = server_.lock()) server->remove(shared_from_this());
    }
private:
    void onRead(Error ec) {
        if (closed_) return;
        if (ec) {
            if (ec == http::error::header_limit) fail(431);
            else if (ec == http::error::body_limit) fail(413);
            else if (ec == http::error::end_of_stream || ec == net::error::operation_aborted ||
                     ec == beast::error::timeout) close();
            else fail(400);
            return;
        }
        const auto& req = parser_->get();
        version_ = req.version();
        keep_alive_ = req.keep_alive();
        head_ = req.method() == http::verb::head;
        if (req.target().empty() || req.target().front() != '/') {
            fail(400);
            return;
        }
        auto server = server_.lock();
        if (!server) { close(); return; }
        waiting_ = true;
        const auto request_id = ++request_id_;
        // tcp_stream 的期限只管理实际 I/O；另设 timer 覆盖等待业务结果的阶段。
        stream_.expires_never();
        business_timer_.expires_after(server->timeout());
        business_timer_.async_wait([self = shared_from_this(), request_id](Error timer_ec) {
            if (!timer_ec && self->waiting_ && self->request_id_ == request_id) {
                self->keep_alive_ = false;
                self->reply(request_id, {504, "application/json", "{\"error\":\"request timeout\"}"});
            }
        });
        std::weak_ptr<Session> weak_self = shared_from_this();
        std::weak_ptr<ReplyBridge> weak_bridge = server->bridge();
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        HttpReply callback = [weak_self, weak_bridge, delivered, request_id](HttpResponse response) {
            if (delivered->exchange(true)) return;
            if (auto bridge = weak_bridge.lock()) {
                bridge->post([weak_self, request_id, response = std::move(response)]() mutable {
                    if (auto self = weak_self.lock()) self->reply(request_id, std::move(response));
                });
            }
        };
        bool accepted = false;
        try {
            accepted = server->handler() && server->handler()(
                {std::string(req.method_string()), std::string(req.target())}, std::move(callback));
        } catch (const std::exception& ex) {
            LOG_ERROR("http submission failed: %s", ex.what());
            keep_alive_ = false;
            reply(request_id, {500, "application/json", "{\"error\":\"request failed\"}"});
            return;
        }
        if (!accepted) {
            // 拒绝的请求不保留连接，防止过载客户端用 pipeline 无限重试。
            keep_alive_ = false;
            reply(request_id, {503, "application/json", "{\"error\":\"server busy\"}"});
        }
    }
    void fail(unsigned status) {
        keep_alive_ = false;
        waiting_ = true;
        reply(++request_id_, {status, "application/json", "{\"error\":\"invalid request\"}"});
    }
    void reply(std::uint64_t request_id, HttpResponse response) {
        if (closed_ || !waiting_ || request_id != request_id_) return;
        waiting_ = false;
        business_timer_.cancel();
        auto server = server_.lock();
        if (!server) { close(); return; }
        stream_.expires_after(server->timeout());
        const auto body_size = response.body.size();
        auto done = [self = shared_from_this()](Error ec, std::size_t) {
            if (ec || !self->keep_alive_ || self->closed_) self->close();
            else {
                self->response_.reset();
                self->head_response_.reset();
                self->read();
            }
        };
        if (head_) {
            head_response_.emplace(static_cast<http::status>(response.status), version_);
            head_response_->set(http::field::server, "edge-gateway/Beast");
            head_response_->set(http::field::content_type, response.content_type);
            head_response_->keep_alive(keep_alive_);
            head_response_->content_length(body_size);
            http::async_write(stream_, *head_response_, std::move(done));
        } else {
            response_.emplace(static_cast<http::status>(response.status), version_);
            response_->set(http::field::server, "edge-gateway/Beast");
            response_->set(http::field::content_type, response.content_type);
            response_->keep_alive(keep_alive_);
            response_->body() = std::move(response.body);
            response_->prepare_payload();
            http::async_write(stream_, *response_, std::move(done));
        }
    }

    beast::tcp_stream stream_;
    net::steady_timer business_timer_;
    beast::flat_buffer buffer_;
    std::weak_ptr<Server> server_;
    std::optional<http::request_parser<http::string_body>> parser_;
    std::optional<http::response<http::string_body>> response_;
    std::optional<http::response<http::empty_body>> head_response_;
    std::uint64_t request_id_ = 0;
    unsigned version_ = 11;
    bool keep_alive_ = false;
    bool head_ = false;
    bool waiting_ = false;
    bool closed_ = false;
};

void Server::accept() {
    acceptor_.async_accept([self = shared_from_this()](Error ec, tcp::socket socket) {
        if (self->stopping_) return;
        if (!ec && self->sessions_.size() < kConnectionLimit) {
            auto session = std::make_shared<Session>(std::move(socket), self);
            self->sessions_.insert(session);
            session->read();
        }
        // 超出连接数上限时 socket 随作用域关闭。资源耗尽等错误不能立即重试，
        // 否则持续的 EMFILE 会让事件循环忙转并刷日志；定时重试不影响现有连接。
        if (ec && ec != net::error::operation_aborted) {
            LOG_WARN("http accept failed: %s", ec.message().c_str());
            self->accept_retry_timer_.expires_after(std::chrono::milliseconds(100));
            self->accept_retry_timer_.async_wait([self](Error timer_ec) {
                if (!timer_ec && !self->stopping_) self->accept();
            });
        } else if (!ec) self->accept();
    });
}

void Server::stop() {
    if (stopping_) return;
    stopping_ = true;
    bridge_->disable();
    Error ignored;
    acceptor_.close(ignored);
    stop_timer_.cancel();
    accept_retry_timer_.cancel();
    // close 会从 sessions_ 删除自身，先转移集合以避免迭代失效。
    auto sessions = std::move(sessions_);
    for (const auto& session : sessions) session->close();
}
}  // namespace

std::optional<HttpResponse> staticHttpResponse(const std::string& path) {
    if (path == "/") return HttpResponse{200, "text/html; charset=utf-8", kIndexHtml};
    if (path == "/uplot.js") return HttpResponse{200, "application/javascript", kUplotJs};
    if (path == "/uplot.css") return HttpResponse{200, "text/css", kUplotCss};
    return std::nullopt;
}

int clampReportN(const std::string& raw, int default_n) {
    const int fallback = std::clamp(default_n, 1, kMaxReportN);
    if (raw.empty()) return fallback;
    try {
        const int n = std::stoi(raw);
        return n < 1 || n > kMaxReportN ? fallback : n;
    } catch (...) {
        return fallback;
    }
}

bool runHttpServer(int port, HttpRuntimeConfigProvider config, HttpRequestHandler handler,
                   std::function<bool()> should_stop,
                   std::function<void(unsigned short)> on_listening) {
    if (port < 0 || port > 65535) return false;
    net::io_context io(1);
    auto server = std::make_shared<Server>(io, std::move(config), std::move(handler),
                                         std::move(should_stop));
    try {
        const auto bound_port = server->listen(port);
        if (on_listening) on_listening(bound_port);
        LOG_INFO("http Beast server on :%u", static_cast<unsigned>(bound_port));
        io.run();
        server->stop();
        LOG_INFO("%s", "http monitor stopped");
        return true;
    } catch (const std::exception& ex) {
        // 先使桥失效、取消所有操作，再销毁 context；队列中的迟到 reply 不持有 session。
        server->stop();
        LOG_ERROR("http server failed: %s", ex.what());
        return false;
    }
}
}  // namespace gateway
