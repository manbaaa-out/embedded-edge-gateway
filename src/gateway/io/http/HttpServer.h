#pragma once

/**
 * @file
 * 内嵌 HTTP 监控服务的装配接口。
 *
 * I/O 层只接收数据库引用、运行期配置快照提供器和停机判据，不依赖 ConfigManager
 * 或信号实现。服务可因此在独立线程内运行，同时按请求/扫描周期看到热加载后的配置。
 */

#include "gateway/core/config/Config.h"   // 提供查询点数的统一上限 kMaxReportN。
#include "gateway/storage/Database.h"

#include <functional>
#include <string>

namespace gateway {

/** 每次读取时形成的 HTTP 可热加载配置快照。 */
struct HttpRuntimeConfig {
    int idle_timeout_s = 5;  ///< 无活动连接的回收阈值，单位为秒。
    int report_n       = 10; ///< /api/data 未提供 n 时的默认返回点数。
};

/** 配置快照提供器；由 HTTP 线程调用，返回值应可安全跨线程读取。 */
using HttpRuntimeConfigProvider = std::function<HttpRuntimeConfig()>;

/**
 * 解析并限制查询点数。
 * @param raw URL 参数 n 的原文；空字符串表示未提供。
 * @param default_n 配置提供的默认点数，也会夹紧到合法区间。
 * @return [1, kMaxReportN] 内的点数；无法解析数值前缀或越界时回退到默认值。
 * std::stoi 接受的数字前缀会沿用，例如 "12abc" 解析为 12。
 */
int clampReportN(const std::string& raw, int default_n);

/**
 * 在调用线程中阻塞运行监控服务。
 *
 * @param roDb 由本线程独占使用的只读连接，通常与 WAL 写连接指向同一数据库。
 * @param port 监听 TCP 端口，范围应已由配置层校验。
 * @param config 运行期配置提供器；扫描连接和处理请求时重新调用。
 * @param should_stop 停机判据；空回调表示持续运行。
 *
 * 初始化失败时记录错误并返回；运行后最多等待下一次 timerfd 扫描观察停机请求。
 */
void runHttpServer(Database& roDb, int port, HttpRuntimeConfigProvider config,
                   std::function<bool()> should_stop);

}  // namespace gateway
