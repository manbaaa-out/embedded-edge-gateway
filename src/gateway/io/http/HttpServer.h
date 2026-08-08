#pragma once
#include "gateway/storage/Database.h"

#include <functional>
#include <string>

namespace gateway {

// HTTP 线程在运行期需要现取的配置项(A 档:改内存即生效)。
//
// 这些值不能在启动时取一次就定死 —— 那等于把 A 档悄悄降级成 C 档:
// 配置改了、SIGHUP 发了、日志说生效了,行为却纹丝不动。
struct HttpRuntimeConfig {
    int idle_timeout_s = 5;   // 空闲连接回收阈值
    int report_n       = 10;  // /api/data 未带 n 时的默认点数
};

// 每次使用时现取上述配置。由调用方注入,使 io 层不必知道配置从哪来 ——
// 与 link / pipeline / cloud 一样,下层只接收参数,不去够全局的 ConfigManager。
using HttpRuntimeConfigProvider = std::function<HttpRuntimeConfig()>;

// /api/data 单次返回的点数上限,防止一次查询把整张表拖进内存。
inline constexpr int kMaxReportN = 1000;

// 把 /api/data 的 n 参数归一到 [1, kMaxReportN]。
//
// raw 为空表示请求未带该参数;非数字或越界一律回落到 default_n,而 default_n
// 自身也会被夹紧 —— 配置只校验 report_n > 0,写 5000 照样能过。
// 之所以独立成函数,是因为它是这条读路径上唯一不碰 I/O 的判断,可以单测。
int clampReportN(const std::string& raw, int default_n);

// 在调用线程中阻塞运行 HTTP 监控服务:epoll Reactor + timerfd 空闲连接回收 + HTTP 解析。
// roDb 须为只读连接,与主链路的写连接配合,依靠 SQLite 的 WAL 模式实现读写并发。
// 本函数内部永久阻塞,通常放在独立 std::thread 中运行。
//
// 两个配置入口的形态差异是有意的,它把配置分档编码进了签名:
//   port   —— C 档,进程生命周期内不变,按值传一次即可;
//   config —— A 档,可在运行期改,故传取值器而非取好的值。
void runHttpServer(Database& roDb, int port, HttpRuntimeConfigProvider config);

}  // namespace gateway
