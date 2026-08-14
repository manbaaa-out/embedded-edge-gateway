// 第二个 EventLoop:监控页与 /api/data。与主 Reactor 完全隔离,只共享一个只读
// SQLite 连接。
//
// 这条线程不知道 ConfigManager 存在:idle_timeout / report_n 由 app 层注入取值器,
// 每次用时现取 —— 取好值传下来就等于把 A 档配置悄悄降级成需要重启的 C 档。
// should_stop 同理:它只知道「有人会告诉我该走了」,不知道那是 SIGTERM。

#include "gateway/io/http/HttpServer.h"
#include "gateway/core/format/Number.h"
#include "gateway/core/log/Logger.h"
#include "gateway/io/event/EventLoop.h"
#include "gateway/io/http/HttpRequest.h"
#include "gateway/io/http/WebAsset.h"   // kIndexHtml 等,由 CMake 在编译期从 assets/ 嵌入

#include <algorithm>
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

// ---- 请求处理 ----

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
        // 数值走 formatValue 而非就地 %.2f:同一个读数在 MQTT 上行、查询应答、
        // 本处 JSON 三个出口必须长得一样,否则对不上账。位数由协议的定标决定。
        snprintf(buf, sizeof(buf),
                 "{\"device_id\":\"%s\",\"value\":%s,\"ts\":%ld}",
                 r.device_id.c_str(), formatValue(r.value).c_str(), r.ts);
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

// 监控页。HTML 在编译期从 assets/index.html 嵌入,部署时无需附带静态文件。
static std::string monitorPage() {
    return kIndexHtml;
}

int clampReportN(const std::string& raw, int default_n) {
    // 默认值先夹紧:report_n 只被校验 > 0,配成 5000 也能过校验,
    // 不夹就等于让配置文件绕过这里的上限。
    const int fallback = std::clamp(default_n, 1, kMaxReportN);
    if (raw.empty()) return fallback;

    int n = 0;
    // stoi 的宽松("12abc" → 12)是此前就有的行为,读接口上无害,故原样保留 ——
    // 与 translateCommand 的严格解析不同:那边的数字要写进设备。
    try {
        n = std::stoi(raw);
    } catch (...) {
        return fallback;
    }
    return (n < 1 || n > kMaxReportN) ? fallback : n;
}

static std::string handleHttpRequest(const std::string& rawPath, Database& roDb,
                                     const HttpRuntimeConfig& rt) {
    std::string path, query;
    splitPathQuery(rawPath, path, query);

    if (path == "/") {
        return makeResponse(200, "OK", "text/html; charset=utf-8", monitorPage());
    }
    // uPlot 静态资源,同样编译期嵌入以保证离线自包含
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

        const auto it = params.find("n");
        const int  n  = clampReportN(it != params.end() ? it->second : std::string(),
                                     rt.report_n);

        auto rows = roDb.query(dev, n);
        return makeResponse(200, "OK", "application/json", rowsToJson(rows));
    }

    return makeResponse(404, "Not Found", "application/json",
                        "{\"error\":\"not found\"}");
}

// ---- 发送辅助 ----
// len 取 size_t 而非 ssize_t:它表示长度,不表示可能为负的返回值;
// 用 ssize_t 会导致每处 len - total 都要与 size_t 来回强转。
static void sendData(EventLoop& loop, channel* ch, const char* data, size_t len) {
    size_t total = 0;
    if (ch->out_buf.empty()) {
        while (total < len) {
            ssize_t n = send(ch->fd, data + total, len - total, MSG_NOSIGNAL);
            if (n > 0)                                 total += static_cast<size_t>(n);
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

// ---- 连接上下文与 fd → 上下文映射表。仅 HTTP 线程访问,故无锁 ----
struct HttpConn {
    Buffer      buf;
    HttpRequest req;
};
static std::map<int, HttpConn> g_conns;

// ---- 服务主循环 ----
void runHttpServer(Database& roDb, int port, HttpRuntimeConfigProvider config,
                   std::function<bool()> should_stop) {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) { LOG_ERROR("http socket() failed: %s", strerror(errno)); return; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));   // 端口范围由配置校验保证
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // 端口一旦可配,bind 失败就成了用户能触发的情形(特权端口、端口被占)。
    // 此前端口写死 8888,失败几乎不可能,故返回值被忽略;现在必须报出来,
    // 否则日志会说"已启动"而实际一个连接都收不到。
    if (bind(listen_fd, (const sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("http bind :%d failed: %s", port, strerror(errno));
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 10) < 0) {
        LOG_ERROR("http listen :%d failed: %s", port, strerror(errno));
        close(listen_fd);
        return;
    }

    // 这两条失败路径此前直接 return,把上面刚 bind + listen 好的 listen_fd 漏在那里
    // (第二条还连 timerfd 一起漏)—— 而同一个函数里 bind/listen 失败却都老实关了,
    // 两种风格并存。timerfd_create 失败在实践中几乎只发生于 fd 耗尽,而那正是最不该
    // 再泄漏一个 fd 的时刻。
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        LOG_ERROR("http timerfd_create failed: %s", strerror(errno));
        close(listen_fd);
        return;
    }
    struct itimerspec value;
    value.it_value.tv_sec = 2;  value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1; value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) {
        LOG_ERROR("http timerfd_settime failed: %s", strerror(errno));
        close(timerfd);
        close(listen_fd);
        return;
    }

    EventLoop loop;

    std::shared_ptr<channel> timer_channel = std::make_shared<channel>();
    timer_channel->fd = timerfd;
    timer_channel->events = EPOLLIN;
    timer_channel->on_read = [timerfd, &loop, &config, &should_stop]() {
        uint64_t exp = 0;
        // 必须读掉计数,否则 LT 模式下会反复触发。读失败不影响后续超时扫描:
        // 扫描依据是各连接的 last_active,不依赖该计数。
        if (read(timerfd, &exp, sizeof(exp)) != static_cast<ssize_t>(sizeof(exp))) {
            LOG_DEBUG("%s", "http timerfd read short");
        }

        // 停机意愿在本线程内检查、由本线程调 quit —— 见 EventLoop::quit 的约定。
        // 放在超时扫描之前:既然要走了,就不必再花时间回收连接。
        if (should_stop && should_stop()) {
            LOG_INFO("%s", "http monitor stopping");
            loop.quit();
            return;
        }
        // A 档:每次扫描现取,改完配置发 SIGHUP 后下一秒即生效,无需重启
        const int timeout_s = config().idle_timeout_s;
        time_t now = time(nullptr);
        std::vector<int> timeout_fds;
        loop.forEachChannel([&](channel* ch) {
            if (ch->fd == timerfd) return;
            if (ch->timeout_exempt) return;
            if (now - ch->last_active > timeout_s) timeout_fds.push_back(ch->fd);
        });
        for (int fd : timeout_fds) {
            LOG_INFO("http fd=%d idle timeout, closing", fd);
            loop.removeChannel(fd);
            g_conns.erase(fd);
        }
    };
    loop.addChannel(timer_channel);

    std::shared_ptr<channel> listen_channel = std::make_shared<channel>();
    listen_channel->fd = listen_fd;
    listen_channel->events = EPOLLIN | EPOLLET;
    listen_channel->timeout_exempt = true;
    listen_channel->on_read = [listen_fd, &loop, &roDb, &config]() {
        while (1) {
            int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                LOG_ERROR("http accept4 failed: %s", strerror(errno));
                break;
            }
            g_conns[client_fd];

            std::shared_ptr<channel> cc = std::make_shared<channel>();
            cc->fd = client_fd;
            cc->events = EPOLLIN | EPOLLET;
            cc->last_active = time(nullptr);
            channel* ch_raw = cc.get();

            cc->on_read = [client_fd, &loop, ch_raw, &roDb, &config]() {
                HttpConn& conn = g_conns[client_fd];
                while (1) {
                    char tmp[4096];
                    ssize_t n_read = recv(client_fd, tmp, sizeof(tmp), 0);
                    if (n_read < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN) break;
                        // 此前是 perror:它绕开 AsyncLogger 同步写 stderr,而这里正是
                        // 每条连接的读路径 —— journald 堵住时会把 HTTP 线程钉在这一行,
                        // 恰恰是异步日志要防的那件事。
                        LOG_WARN("http recv failed on fd=%d: %s", client_fd, strerror(errno));
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else if (n_read == 0) {
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else {
                        ch_raw->last_active = time(nullptr);
                        conn.buf.append(tmp, static_cast<size_t>(n_read));   // n_read > 0
                        while (true) {
                            ParseResult r = conn.req.parse(&conn.buf);
                            if (r == ParseResult::kComplete) {
                                // 每个请求现取一次 A 档配置,而非启动时定死
                                std::string resp =
                                    handleHttpRequest(conn.req.path(), roDb, config());
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
                    if (n > 0) ch_raw->out_buf.erase(0, static_cast<size_t>(n));
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

    LOG_INFO("http monitor server on :%d (open http://<ip>:%d/ in browser)", port, port);
    loop.loop();

    // 循环退出后 loop 析构,各 channel 随之 close 掉 listen_fd / timerfd / 连接 fd。
    // g_conns 是文件作用域的,不随 loop 走,得手工清 —— 否则再起一次服务会看到
    // 上一次遗留的连接上下文。
    g_conns.clear();
    LOG_INFO("%s", "http monitor stopped");
}

} // namespace gateway