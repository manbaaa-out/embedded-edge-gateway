#pragma once

#include "gateway/core/config/Config.h"

#include <functional>
#include <optional>
#include <string>

namespace gateway {

/** HTTP 网络循环每次开始读、写或等待业务结果时读取的配置快照。 */
struct HttpRuntimeConfig {
    int idle_timeout_s = 5;
    int report_n = 10;
};
using HttpRuntimeConfigProvider = std::function<HttpRuntimeConfig()>;

/** 拥有独立存储的应用请求；不能把 Beast parser 或 socket 借给业务线程。 */
struct HttpRequest {
    std::string method;
    std::string target;
};
struct HttpResponse {
    unsigned status = 200;
    std::string content_type = "application/json";
    std::string body;
};
using HttpReply = std::function<void(HttpResponse)>;
/**
 * HTTP Reactor 调用此入口投递请求，不应在入口执行路由、SQL 或等待结果。
 * 返回 false 表示业务队列拒绝接收，由 HTTP 层立即响应 503。
 * 返回 true 后可从任意线程调用 reply；重复、超时或停机后的回复安全丢弃。
 */
using HttpRequestHandler = std::function<bool(HttpRequest, HttpReply)>;

/** 业务层使用的嵌入资源映射；未知路径返回 nullopt。 */
std::optional<HttpResponse> staticHttpResponse(const std::string& path);

/** 数字前缀沿用 std::stoi 语义，非法/越界值回退到夹紧后的默认值。 */
int clampReportN(const std::string& raw, int default_n);

/**
 * 在调用线程运行独立的 Boost.Asio / Beast HTTP/1 网络循环。
 * 连接、解析、收发和超时仅由该线程访问；业务回复通过 post 交回。
 * header/body 分别限制为 16/64 KiB，同时最多接受 256 个连接。
 * 每个连接最多有一个业务请求在途，流水线字节留在有界接收缓冲中。
 * should_stop 每 100ms 检查一次；正常停止返回 true，初始化/运行失败返回 false。
 * port=0 和 on_listening 供集成测试使用，回调收到实际绑定的端口。
 */
bool runHttpServer(int port, HttpRuntimeConfigProvider config, HttpRequestHandler handler,
                   std::function<bool()> should_stop,
                   std::function<void(unsigned short)> on_listening = {});

}  // namespace gateway
