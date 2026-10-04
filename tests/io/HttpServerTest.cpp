// clampReportN 的纯函数边界测试。第一个参数是 URL 查询值文本，第二个参数是配置的
// report_n；返回值最终进入 SQLite LIMIT，因此任何路径都必须落在 [1, kMaxReportN]。

#include "gateway/io/http/HttpServer.h"

#include <gtest/gtest.h>

using gateway::clampReportN;
using gateway::kMaxReportN;

// 查询参数缺失时直接使用配置默认值，确保运行期配置确实影响接口行为。
TEST(ClampReportN, AbsentParamFallsBackToConfiguredDefault) {
    EXPECT_EQ(clampReportN("", 10), 10);
    EXPECT_EQ(clampReportN("", 50), 50) << "配置改成 50 就该是 50,不能永远是 10";
}

// 合法请求参数优先于配置，两个端点和中间值均保持原值。
TEST(ClampReportN, ValidParamWins) {
    EXPECT_EQ(clampReportN("1", 10), 1);
    EXPECT_EQ(clampReportN("200", 10), 200);
    EXPECT_EQ(clampReportN("1000", 10), kMaxReportN) << "上限本身是合法值";
}

// 非法请求值回退到配置值，不直接参与查询。
TEST(ClampReportN, OutOfRangeAndGarbageFallBack) {
    EXPECT_EQ(clampReportN("0", 10), 10);
    EXPECT_EQ(clampReportN("-5", 10), 10);
    EXPECT_EQ(clampReportN("1001", 10), 10) << "刚过上限";
    EXPECT_EQ(clampReportN("99999999999999999999", 10), 10) << "stoi 抛 out_of_range";
    EXPECT_EQ(clampReportN("abc", 10), 10);
    EXPECT_EQ(clampReportN("  ", 10), 10);
}

// 当前接口保留 stoi 接受数字前缀的行为；该用例明确锁定兼容性边界。
TEST(ClampReportN, LeadingNumberIsAcceptedAsBefore) {
    EXPECT_EQ(clampReportN("12abc", 10), 12);
}

// 防御性夹紧配置值，确保所有回退路径仍满足查询上限。
TEST(ClampReportN, ConfiguredDefaultIsItselfClamped) {
    EXPECT_EQ(clampReportN("", 5000), kMaxReportN);
    EXPECT_EQ(clampReportN("abc", 5000), kMaxReportN) << "回落路径同样要夹紧";
    EXPECT_EQ(clampReportN("", 0), 1) << "校验拦得住 0,此处仍不放行非法值";
    EXPECT_EQ(clampReportN("", -1), 1);
}

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

class RunningHttpServer {
public:
    explicit RunningHttpServer(gateway::HttpRequestHandler handler, int timeout = 2) {
        auto ready = std::make_shared<std::promise<unsigned short>>();
        auto future = ready->get_future();
        thread_ = std::thread([this, handler = std::move(handler), timeout, ready]() {
            result_ = gateway::runHttpServer(0,
                [timeout] { return gateway::HttpRuntimeConfig{timeout, 10}; }, handler,
                [this] { return stop_.load(); },
                [ready](unsigned short port) { ready->set_value(port); });
        });
        if (future.wait_for(2s) != std::future_status::ready) {
            stop();
            throw std::runtime_error("HTTP test server did not listen");
        }
        port_ = future.get();
    }
    ~RunningHttpServer() { stop(); }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    unsigned short port() const { return port_; }
    bool result() const { return result_; }
private:
    std::atomic<bool> stop_{false};
    bool result_ = false;
    unsigned short port_ = 0;
    std::thread thread_;
};

class Client {
public:
    explicit Client(unsigned short port) : socket_(io_) {
        socket_.connect({net::ip::make_address("127.0.0.1"), port});
    }
    void send(const std::string& bytes) { net::write(socket_, net::buffer(bytes)); }
    http::response<http::string_body> read(bool head = false) {
        http::response_parser<http::string_body> parser;
        parser.skip(head);
        net::steady_timer timer(io_, 4s);
        boost::system::error_code result;
        timer.async_wait([this](boost::system::error_code ec) {
            if (!ec) socket_.cancel();
        });
        http::async_read(socket_, buffer_, parser,
            [&timer, &result](boost::system::error_code ec, std::size_t) {
                result = ec;
                timer.cancel();
            });
        io_.restart();
        io_.run();
        if (result) throw boost::system::system_error(result);
        return parser.release();
    }
    bool closed() {
        char byte;
        boost::system::error_code ec;
        net::steady_timer timer(io_, 4s);
        timer.async_wait([this](boost::system::error_code timer_ec) {
            if (!timer_ec) socket_.cancel();
        });
        socket_.async_read_some(net::buffer(&byte, 1),
            [&timer, &ec](boost::system::error_code read_ec, std::size_t) {
                ec = read_ec;
                timer.cancel();
            });
        io_.restart();
        io_.run();
        return ec == net::error::eof || ec == net::error::connection_reset;
    }
private:
    net::io_context io_;
    tcp::socket socket_;
    beast::flat_buffer buffer_;
};

gateway::HttpRequestHandler echoHandler() {
    return [](gateway::HttpRequest request, gateway::HttpReply reply) {
        reply({200, "text/plain", request.method + " " + request.target});
        return true;
    };
}

class PendingReply {
public:
    void store(gateway::HttpReply reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        reply_ = std::move(reply);
        cv_.notify_all();
    }
    gateway::HttpReply get() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, 2s, [this] { return static_cast<bool>(reply_); })) return {};
        return reply_;
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    gateway::HttpReply reply_;
};
}  // namespace

TEST(HttpBeast, FragmentedRequestsAndPipelinedKeepAlivePreserveOrder) {
    RunningHttpServer server(echoHandler());
    Client client(server.port());
    client.send("GET /first HTTP/1.1\r\nHost: local");
    client.send("host\r\n\r\nGET /second HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto first = client.read();
    EXPECT_EQ(first.result_int(), 200u);
    EXPECT_TRUE(first.keep_alive());
    EXPECT_EQ(first.body(), "GET /first");
    EXPECT_EQ(client.read().body(), "GET /second");
    client.send("GET /last HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto last = client.read();
    EXPECT_EQ(last.body(), "GET /last");
    EXPECT_FALSE(last.keep_alive());
    EXPECT_TRUE(client.closed());
}

TEST(HttpBeast, HeadAdvertisesGetBodySizeAndSendsNoBody) {
    RunningHttpServer server([](gateway::HttpRequest, gateway::HttpReply reply) {
        reply({200, "text/plain", "sensor-body"});
        return true;
    });
    Client client(server.port());
    client.send("HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
                "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto head = client.read(true);
    EXPECT_EQ(head[http::field::content_length], "11");
    EXPECT_TRUE(head.body().empty());
    EXPECT_EQ(client.read().body(), "sensor-body");
}

TEST(HttpBeast, FixedAndChunkedBodiesDoNotDesynchronizePipeline) {
    RunningHttpServer server(echoHandler());
    Client client(server.port());
    client.send("POST /fixed HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhe");
    client.send("lloPOST /chunked HTTP/1.1\r\nHost: localhost\r\n"
                "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
                "GET /following HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(client.read().body(), "POST /fixed");
    EXPECT_EQ(client.read().body(), "POST /chunked");
    EXPECT_EQ(client.read().body(), "GET /following");
}

TEST(HttpBeast, WaitingForBusinessDoesNotBlockAnotherConnection) {
    PendingReply pending;
    RunningHttpServer server([&pending](gateway::HttpRequest request, gateway::HttpReply reply) {
        if (request.target == "/slow") pending.store(std::move(reply));
        else reply({200, "text/plain", "fast"});
        return true;
    });
    Client slow(server.port());
    slow.send("GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto reply = pending.get();
    ASSERT_TRUE(reply);
    Client fast(server.port());
    fast.send("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(fast.read().body(), "fast");
    // 从测试线程完成业务；响应必须 post 回 HTTP 所属线程。
    reply({200, "text/plain", "completed"});
    reply({200, "text/plain", "duplicate-must-not-be-sent"});
    EXPECT_EQ(slow.read().body(), "completed");
    slow.send("GET /fast HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(slow.read().body(), "fast");
}

TEST(HttpBeast, QueueRejectionReturns503AndCloses) {
    RunningHttpServer server([](gateway::HttpRequest, gateway::HttpReply) { return false; });
    Client client(server.port());
    client.send("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(client.read().result_int(), 503u);
    EXPECT_TRUE(client.closed());
}

TEST(HttpBeast, BusinessTimeoutReturns504AndLateReplyIsIgnored) {
    PendingReply pending;
    RunningHttpServer server([&pending](gateway::HttpRequest, gateway::HttpReply reply) {
        pending.store(std::move(reply));
        return true;
    }, 1);
    Client client(server.port());
    client.send("GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto reply = pending.get();
    ASSERT_TRUE(reply);
    EXPECT_EQ(client.read().result_int(), 504u);
    reply({200, "text/plain", "too late"});
    EXPECT_TRUE(client.closed());
}

TEST(HttpBeast, ShutdownInvalidatesRepliesBeforeIoContextDestruction) {
    PendingReply pending;
    RunningHttpServer server([&pending](gateway::HttpRequest, gateway::HttpReply reply) {
        pending.store(std::move(reply));
        return true;
    });
    Client client(server.port());
    client.send("GET /pending HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto reply = pending.get();
    ASSERT_TRUE(reply);
    server.stop();
    EXPECT_TRUE(server.result());
    reply({200, "text/plain", "late result after server thread joined"});
    EXPECT_TRUE(client.closed());
}

TEST(HttpBeast, MalformedHeaderAndOversizedRequestsAreRejectedBeforeBusiness) {
    std::atomic<int> submissions{0};
    RunningHttpServer server([&submissions](gateway::HttpRequest, gateway::HttpReply reply) {
        ++submissions;
        reply({200, "text/plain", "unexpected"});
        return true;
    });
    {
        Client client(server.port());
        client.send("GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: nope\r\n\r\n");
        EXPECT_EQ(client.read().result_int(), 400u);
    }
    {
        Client client(server.port());
        client.send("GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: " +
                    std::string(17 * 1024, 'x') + "\r\n\r\n");
        EXPECT_EQ(client.read().result_int(), 431u);
    }
    {
        Client client(server.port());
        client.send("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 65537\r\n\r\n");
        EXPECT_EQ(client.read().result_int(), 413u);
    }
    EXPECT_EQ(submissions.load(), 0);
}

TEST(HttpBeast, IncompleteHeaderTimesOutWithoutBlockingAnotherConnection) {
    RunningHttpServer server(echoHandler(), 1);
    Client incomplete(server.port());
    incomplete.send("GET / HTTP/1.1\r\nHost:");
    Client fast(server.port());
    fast.send("GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(fast.read().body(), "GET /ok");
    EXPECT_TRUE(incomplete.closed());
}

TEST(HttpBeast, IndependentServersAndBindFailureHaveSeparateLifetimes) {
    RunningHttpServer first(echoHandler());
    RunningHttpServer second(echoHandler());
    EXPECT_FALSE(gateway::runHttpServer(first.port(), {}, echoHandler(), [] { return true; }));
    Client first_client(first.port());
    Client second_client(second.port());
    first_client.send("GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n");
    second_client.send("GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(first_client.read().body(), "GET /one");
    EXPECT_EQ(second_client.read().body(), "GET /two");
    first.stop();
    second_client.send("GET /still-alive HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(second_client.read().body(), "GET /still-alive");
}

TEST(HttpBeast, EmbeddedAssetLookupRemainsAvailableToBusinessThread) {
    const auto page = gateway::staticHttpResponse("/");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200u);
    EXPECT_NE(page->body.find("<html"), std::string::npos);
    EXPECT_TRUE(gateway::staticHttpResponse("/uplot.js"));
    EXPECT_TRUE(gateway::staticHttpResponse("/uplot.css"));
    EXPECT_FALSE(gateway::staticHttpResponse("/missing"));
}
