/**
 * @file
 * 内嵌监控服务实现。
 *
 * 服务拥有独立 EventLoop 和线程内连接表，通过连接提供器为每次数据请求取得当前
 * 只读 SQLite 连接。监听 socket 与客户端 socket 使用边沿触发并在回调中读到
 * EAGAIN；timerfd 周期检查热加载配置、空闲连接和停机条件。前端资源在配置阶段
 * 嵌入二进制。
 */

#include "gateway/io/http/HttpServer.h"
#include "gateway/core/format/Number.h"
#include "gateway/core/log/Logger.h"
#include "gateway/io/event/EventLoop.h"
#include "gateway/io/http/HttpRequest.h"
#include "gateway/io/http/WebAsset.h"

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

/**
 * 将 request-target 拆成路径与原始查询串。
 * @param full 包含可选 '?' 的 request-target。
 * @param path 输出 '?' 之前的路径。
 * @param query 输出 '?' 之后的查询串，不包含问号。
 */
static void splitPathQuery(const std::string& full,
                           std::string& path, std::string& query) {
    size_t qmark = full.find('?');  // 第一个查询串分隔符位置。
    if (qmark == std::string::npos) {
        path  = full;
        query = "";
    } else {
        path  = full.substr(0, qmark);
        query = full.substr(qmark + 1);
    }
}

/**
 * 解析本服务使用的简单 key=value 查询串，不执行 URL 解码。
 * @param query 不含前导 '?' 的原始查询串。
 * @return 参数名到参数值的映射；无等号片段被忽略，重复键保留最后一个值。
 */
static std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> params;  // 已解析参数，按键有序存储。
    size_t start = 0;  // 当前 key=value 片段的起始偏移。
    while (start < query.size()) {
        size_t amp = query.find('&', start);  // 当前片段末尾的 '&' 位置。
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(start, amp - start);  // 单个原始参数片段。
        size_t eq = pair.find('=');  // 参数名和值的分隔位置。
        if (eq != std::string::npos) {
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        start = amp + 1;
    }
    return params;
}

/**
 * 将查询结果编码为紧凑 JSON 数组。
 * @param rows 数据库返回的行，顺序保持不变。
 * @return 可直接作为 HTTP body 的 JSON 文本。
 * @pre device_id 来自网关内部固定标识，不含需要 JSON 转义的字符。
 */
static std::string rowsToJson(const std::vector<DataRow>& rows) {
    std::string json = "[";  // 逐行追加，最后补闭合方括号。
    for (size_t i = 0; i < rows.size(); ++i) {  // i 同时用于控制逗号分隔。
        const auto& r = rows[i];  // 当前数据库行的只读引用。
        char buf[256];  // 单个固定字段对象的格式化缓冲区。
        // 与 MQTT 上行共用数值格式，避免不同出口出现精度差异。
        snprintf(buf, sizeof(buf),
                 "{\"device_id\":\"%s\",\"value\":%s,\"ts\":%ld}",
                 r.device_id.c_str(), formatValue(r.value).c_str(), r.ts);
        json += buf;
        if (i + 1 < rows.size()) json += ",";
    }
    json += "]";
    return json;
}

/**
 * 组装一条带 Content-Length 的 HTTP/1.1 keep-alive 响应。
 * @param code 三位状态码。
 * @param reason 与状态码配套的原因短语。
 * @param contentType 响应媒体类型。
 * @param body 原样附加的响应体。
 * @return 完整响应头和响应体字节。
 */
static std::string makeResponse(int code, const std::string& reason,
                                const std::string& contentType,
                                const std::string& body) {
    std::string resp =  // 在单一字符串中生成便于非阻塞发送的完整响应。
        "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: keep-alive\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

/** @return 配置阶段嵌入二进制的监控页 HTML。 */
static std::string monitorPage() {
    return kIndexHtml;
}

int clampReportN(const std::string& raw, int default_n) {
    const int fallback = std::clamp(default_n, 1, kMaxReportN);  // 始终合法的回退值。
    if (raw.empty()) return fallback;

    int n = 0;  // URL 参数解析出的候选点数。
    try {
        n = std::stoi(raw);
    } catch (...) {
        return fallback;
    }
    return (n < 1 || n > kMaxReportN) ? fallback : n;
}

/**
 * 路由一条已解析请求并生成响应。
 * @param rawPath 包含查询串的 request-target。
 * @param database 返回本次请求应使用的只读数据库连接。
 * @param rt 本次请求使用的运行期配置快照。
 * @return 完整 HTTP 响应。
 */
static std::string handleHttpRequest(const std::string& rawPath,
                                     const HttpDatabaseProvider& database,
                                     const HttpRuntimeConfig& rt) {
    std::string path, query;  // 拆分后的路由路径和原始查询串。
    splitPathQuery(rawPath, path, query);

    if (path == "/") {
        return makeResponse(200, "OK", "text/html; charset=utf-8", monitorPage());
    }
    if (path == "/uplot.js") {
        return makeResponse(200, "OK", "application/javascript", kUplotJs);
    }
    if (path == "/uplot.css") {
        return makeResponse(200, "OK", "text/css", kUplotCss);
    }

    if (path == "/api/data") {
        auto params = parseQuery(query);  // API 查询参数集合。

        std::string dev = params.count("dev") ? params["dev"] : "";  // 逻辑设备标识。
        if (dev.empty()) {
            return makeResponse(400, "Bad Request", "application/json",
                                "{\"error\":\"missing dev param\"}");
        }

        const auto it = params.find("n");  // 可选的返回点数参数。
        const int n = clampReportN(  // 最终传给数据库 LIMIT 的合法点数。
            it != params.end() ? it->second : std::string(), rt.report_n);

        // ro_db 的局部 shared_ptr 把本次查询固定在同一连接上；并发热切换只影响后续请求。
        auto ro_db = database ? database() : nullptr;
        if (!ro_db) {
            return makeResponse(503, "Service Unavailable", "application/json",
                                "{\"error\":\"database unavailable\"}");
        }
        auto rows = ro_db->query(dev, n);  // 按时间倒序返回的最近读数。
        return makeResponse(200, "OK", "application/json", rowsToJson(rows));
    }

    return makeResponse(404, "Not Found", "application/json",
                        "{\"error\":\"not found\"}");
}

/**
 * 尽量立即发送一段数据，并把短写后的后缀排入 channel 输出缓冲。
 * @param loop 拥有 ch 的 HTTP 事件循环，用于更新 EPOLLOUT 注册。
 * @param ch 目标连接，必须仍处于活动状态。
 * @param data 待发送字节的起点。
 * @param len 待发送长度，单位为字节。
 * 首次 send 的致命错误会直接返回，连接由后续空闲扫描回收。
 */
static void sendData(EventLoop& loop, channel* ch, const char* data, size_t len) {
    size_t total = 0;  // 本次调用已经直接写入 socket 的字节数。
    if (ch->out_buf.empty()) {
        while (total < len) {
            ssize_t n = send(  // 当前一次非阻塞 send 的结果。
                ch->fd, data + total, len - total, MSG_NOSIGNAL);
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

/** 每条客户端连接独立持有的读缓冲和当前请求解析器。 */
struct HttpConn {
    Buffer      buf;  ///< 跨多次 recv 保留的未解析字节。
    HttpRequest req;  ///< 当前请求的增量解析状态。
};
static std::map<int, HttpConn> g_conns;  ///< fd 到连接上下文；仅 HTTP 线程访问。

void runHttpServer(HttpDatabaseProvider database, int port, HttpRuntimeConfigProvider config,
                   std::function<bool()> should_stop) {
    int listen_fd = socket(  // 监听所有 IPv4 地址的非阻塞 TCP socket。
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) { LOG_ERROR("http socket() failed: %s", strerror(errno)); return; }
    int opt = 1;  // SO_REUSEADDR 的启用值。
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;  // 监听地址：0.0.0.0:<port>。
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

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

    int timerfd = timerfd_create(  // 停机轮询与空闲连接扫描的周期事件源。
        CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        LOG_ERROR("http timerfd_create failed: %s", strerror(errno));
        close(listen_fd);
        return;
    }
    struct itimerspec value;  // 首次 2 秒后触发，随后每秒触发。
    value.it_value.tv_sec = 2;  value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1; value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) {
        LOG_ERROR("http timerfd_settime failed: %s", strerror(errno));
        close(timerfd);
        close(listen_fd);
        return;
    }

    EventLoop loop;  // 本线程独占的 HTTP Reactor。

    std::shared_ptr<channel> timer_channel =  // timerfd 的生命周期和读回调。
        std::make_shared<channel>();
    timer_channel->fd = timerfd;
    timer_channel->events = EPOLLIN;
    // 定时回调先消费 timerfd，再处理停机请求并回收超过动态阈值的客户端连接。
    timer_channel->on_read = [timerfd, &loop, &config, &should_stop]() {
        uint64_t exp = 0;  // 自上次读取以来累积的定时器到期次数。
        // 清空 timerfd 可读状态；超时判断只依赖连接时间戳。
        if (read(timerfd, &exp, sizeof(exp)) != static_cast<ssize_t>(sizeof(exp))) {
            LOG_DEBUG("%s", "http timerfd read short");
        }

        if (should_stop && should_stop()) {
            LOG_INFO("%s", "http monitor stopping");
            loop.quit();
            return;
        }
        // 每轮扫描重新读取运行期配置。
        const int timeout_s = config().idle_timeout_s;  // 本轮使用的空闲阈值，单位秒。
        time_t now = time(nullptr);  // 本轮扫描共享的当前 Unix 秒。
        std::vector<int> timeout_fds;  // 扫描后统一删除，避免遍历时修改活动表。
        // ch 是活动表中的借用事件源；这里只读取，不在遍历期间修改容器。
        loop.forEachChannel([&](channel* ch) {
            if (ch->fd == timerfd) return;
            if (ch->timeout_exempt) return;
            if (now - ch->last_active > timeout_s) timeout_fds.push_back(ch->fd);
        });
        for (int fd : timeout_fds) {  // fd 是已超过空闲阈值的客户端描述符。
            LOG_INFO("http fd=%d idle timeout, closing", fd);
            loop.removeChannel(fd);
            g_conns.erase(fd);
        }
    };
    loop.addChannel(timer_channel);

    std::shared_ptr<channel> listen_channel =  // 监听 socket 的所有权和 accept 回调。
        std::make_shared<channel>();
    listen_channel->fd = listen_fd;
    listen_channel->events = EPOLLIN | EPOLLET;
    listen_channel->timeout_exempt = true;
    // 监听回调在边沿触发模式下持续 accept，直至队列返回 EAGAIN。
    listen_channel->on_read = [listen_fd, &loop, &database, &config]() {
        while (1) {
            int client_fd = accept4(  // 边沿触发下持续接收，直至返回 EAGAIN。
                listen_fd, NULL, NULL, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                LOG_ERROR("http accept4 failed: %s", strerror(errno));
                break;
            }
            g_conns[client_fd];  // 为新 fd 值初始化独立的 Buffer 和 HttpRequest。

            std::shared_ptr<channel> cc =  // 新连接的 fd 所有权和读写回调。
                std::make_shared<channel>();
            cc->fd = client_fd;
            cc->events = EPOLLIN | EPOLLET;
            cc->last_active = time(nullptr);
            channel* ch_raw = cc.get();  // 回调借用；EventLoop/dying_ 保证生命周期。

            // 读回调排空 socket，将字节追加到连接缓冲，并连续处理完整的流水线请求。
            cc->on_read = [client_fd, &loop, ch_raw, &database, &config]() {
                HttpConn& conn = g_conns[client_fd];  // 当前 fd 的持久解析上下文。
                while (1) {
                    char tmp[4096];  // 单次 recv 的栈缓冲，内容立即追加到 conn.buf。
                    ssize_t n_read = recv(  // 当前 recv 的字节数或错误状态。
                        client_fd, tmp, sizeof(tmp), 0);
                    if (n_read < 0) {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN) break;
                        LOG_WARN("http recv failed on fd=%d: %s", client_fd, strerror(errno));
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else if (n_read == 0) {
                        loop.removeChannel(client_fd); g_conns.erase(client_fd); return;
                    } else {
                        ch_raw->last_active = time(nullptr);
                        conn.buf.append(tmp, static_cast<size_t>(n_read));
                        while (true) {
                            ParseResult r = conn.req.parse(&conn.buf);  // 当前请求解析进度。
                            if (r == ParseResult::kComplete) {
                                std::string resp =  // 当前请求对应的完整响应字节。
                                    handleHttpRequest(conn.req.path(), database, config());
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
            // 写回调尽量排空待发缓冲；EAGAIN 保留 EPOLLOUT，空缓冲则取消监听。
            // 其他 send 结果同样保留缓冲，最终由空闲扫描回收连接。
            cc->on_write = [client_fd, &loop, ch_raw]() {
                while (!ch_raw->out_buf.empty()) {
                    ssize_t n = send(  // 尝试清空此前短写留下的输出前缀。
                        client_fd, ch_raw->out_buf.data(),
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

    // channel 随 EventLoop 析构关闭 fd；文件作用域的上下文表需单独清空。
    g_conns.clear();
    LOG_INFO("%s", "http monitor stopped");
}

}  // namespace gateway
