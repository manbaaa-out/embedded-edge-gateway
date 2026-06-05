#include "HttpServer.h"
#include "EventLoop.h"
#include "HttpRequest.h"   // gateway::HttpRequest, gateway::Buffer
#include "WebAsset.h"      // [S6] kIndexHtml(CMake 编译期从 web/index.html 嵌入)

#include <map>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <memory>

namespace gateway {

// ============================================================
// 一、HTTP 业务处理辅助函数(static = 文件内私有)
// ============================================================

static void splitPathQuery(const std::string& full,
                           std::string& path, std::string& query) {
    size_t qmark = full.find('?');
    if (qmark == std::string::npos) {
        path  = full;
        query = "";
    } else {
        path  = full.substr(0, qmark);
        query = full.substr(qmark + 1);
    }
}

static std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start < query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(start, amp - start);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        start = amp + 1;
    }
    return params;
}

static std::string rowsToJson(const std::vector<DataRow>& rows) {
    std::string json = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"device_id\":\"%s\",\"value\":%.2f,\"ts\":%ld}",
                 r.device_id.c_str(), r.value, r.ts);
        json += buf;
        if (i + 1 < rows.size()) json += ",";
    }
    json += "]";
    return json;
}

static std::string makeResponse(int code, const std::string& reason,
                                const std::string& contentType,
                                const std::string& body) {
    std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: keep-alive\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

// [S6] 监控页:返回编译期从 web/index.html 嵌入的 HTML(asset embedding)。
static std::string monitorPage() {
    return kIndexHtml;
}

static std::string handleHttpRequest(const std::string& rawPath, Database& roDb) {
    std::string path, query;
    splitPathQuery(rawPath, path, query);

    if (path == "/") {
        return makeResponse(200, "OK", "text/html; charset=utf-8", monitorPage());
    }
    // [S6] uPlot 静态资源(编译期嵌入,离线自包含)
    if (path == "/uplot.js") {
        return makeResponse(200, "OK", "application/javascript", kUplotJs);
    }
    if (path == "/uplot.css") {
        return makeResponse(200, "OK", "text/css", kUplotCss);
    }

    if (path == "/api/data") {
        auto params = parseQuery(query);

        std::string dev = params.count("dev") ? params["dev"] : "";
        if (dev.empty()) {
            return makeResponse(400, "Bad Request", "application/json",
                                "{\"error\":\"missing dev param\"}");
        }

        int n = 10;
        if (params.count("n")) {
            try { n = std::stoi(params["n"]); } catch (...) { n = 10; }
        }
        if (n <= 0 || n > 1000) n = 10;

        auto rows = roDb.query(dev, n);
        return makeResponse(200, "OK", "application/json", rowsToJson(rows));
    }

    return makeResponse(404, "Not Found", "application/json",
                        "{\"error\":\"not found\"}");
}

// ============================================================
// 二、发送辅助
// ============================================================
static void sendData(EventLoop& loop, channel* ch, const char* data, ssize_t len) {
    ssize_t total = 0;
    if (ch->out_buf.empty()) {
        while (total < len) {
            ssize_t n = send(ch->fd, data + total, len - total, MSG_NOSIGNAL);
            if (n > 0)                                 total += n;
            else if (n < 0 && errno == EINTR)          continue;
            else if (n < 0 && errno == EAGAIN)         break;
            else                                       return;
        }
    }
    if (total < len) {
        ch->out_buf.append(data + total, len - total);
        if (!(ch->events & EPOLLOUT)) {
            ch->events |= EPOLLOUT;
            loop.modifyChannel(ch);
        }
    }
}

// ============================================================
// 三、连接上下文 + fd→上下文 外挂表(仅 HTTP 线程访问,无锁)
// ============================================================
struct HttpConn {
    Buffer      buf;
    HttpRequest req;
};
static std::map<int, HttpConn> g_conns;

// ============================================================
// 四、runHttpServer
// ============================================================
void runHttpServer(Database& roDb) {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (const sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) { perror("timerfd_create"); return; }
    struct itimerspec value;
    value.it_value.tv_sec = 2;  value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1; value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) { perror("settime"); return; }

    EventLoop loop;
    const int TIMEOUT = 5;

    std::shared_ptr<channel> timer_channel = std::make_shared<channel>();
    timer_channel->fd = timerfd;
    timer_channel->events = EPOLLIN;
    timer_channel->on_read = [timerfd, &loop, TIMEOUT]() {
        uint64_t exp = 0;
        read(timerfd, &exp, sizeof(exp));
        time_t now = time(nullptr);
        std::vector<int> timeout_fds;
        loop.forEachChannel([&](channel* ch) {
            if (ch->fd == timerfd) return;
            if (ch->timeout_exempt) return;
            if (now - ch->last_active > TIMEOUT) timeout_fds.push_back(ch->fd);
        });
        for (int fd : timeout_fds) {
            printf("[http][timer] fd=%d idle timeout, closing\n", fd);
            loop.removeChannel(fd);
            g_conns.erase(fd);
        }
    };
    loop.addChannel(timer_channel);

    std::shared_ptr<channel> listen_channel = std::make_shared<channel>();
    listen_channel->fd = listen_fd;
    listen_channel->events = EPOLLIN | EPOLLET;
    listen_channel->timeout_exempt = true;
    listen_channel->on_read = [listen_fd, &loop, &roDb]() {
        while (1) {
            int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                perror("accept4"); break;
            }
            g_conns[client_fd];

            std::shared_ptr<channel> cc = std::make_shared<channel>();
            cc->fd = client_fd;
            cc->events = EPOLLIN | EPOLLET;
            cc->last_active = time(nullptr);
            channel* ch_raw = cc.get();

            cc->on_read = [client_fd, &loop, ch_raw, &roDb]() {
                HttpConn& conn = g_conns[client_fd];
                while (1) {
                    char tmp[4096];
                    ssize_t n_read = recv(client_fd, tmp, sizeof(tmp), 0);
                    if (n_read < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN) break;
                        perror("recv");
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else if (n_read == 0) {
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else {
                        ch_raw->last_active = time(nullptr);
                        conn.buf.append(tmp, n_read);
                        while (true) {
                            ParseResult r = conn.req.parse(&conn.buf);
                            if (r == ParseResult::kComplete) {
                                std::string resp = handleHttpRequest(conn.req.path(), roDb);
                                sendData(loop, ch_raw, resp.data(), resp.size());
                                conn.req.reset();
                            } else if (r == ParseResult::kIncomplete) {
                                break;
                            } else {
                                loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                            }
                        }
                    }
                }
            };
            cc->on_write = [client_fd, &loop, ch_raw]() {
                while (!ch_raw->out_buf.empty()) {
                    ssize_t n = send(client_fd, ch_raw->out_buf.data(),
                                     ch_raw->out_buf.size(), MSG_NOSIGNAL);
                    if (n > 0) ch_raw->out_buf.erase(0, n);
                    else if (n < 0 && errno == EINTR) continue;
                    else if (n < 0 && errno == EAGAIN) break;
                    else break;
                }
                if (ch_raw->out_buf.empty()) {
                    ch_raw->events &= ~EPOLLOUT;
                    loop.modifyChannel(ch_raw);
                }
            };
            loop.addChannel(cc);
        }
    };
    loop.addChannel(listen_channel);

    printf("[http] monitor server on :8888  (open http://<ip>:8888/ in browser)\n");
    loop.loop();
}

} // namespace gateway