#pragma once
#include "gateway/core/config/Config.h"   // 仅为 kMaxReportN,见下方说明
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

// kMaxReportN(/api/data 单次返回的点数上限,防止一次查询把整张表拖进内存)
// 定义在 core/config/Config.h —— 它同时是配置项 report_n 的取值上限,而一个字段
// 的合法范围该和字段待在一起。此处只包含那个头文件取常量,不使用 ConfigManager:
// io 层依旧不知道配置从哪来,那是 app 层的事。

// 把 /api/data 的 n 参数归一到 [1, kMaxReportN]。
//
// raw 为空表示请求未带该参数;非数字或越界一律回落到 default_n。default_n 仍然
// 要夹一次:配置侧现在会拦下越界的 report_n,但这个函数的另一个入口是 URL 上
// 不可信的 ?n= 参数,不夹不行 —— 两个入口共用上限,校验各做各的。
// 之所以独立成函数,是因为它是这条读路径上唯一不碰 I/O 的判断,可以单测。
int clampReportN(const std::string& raw, int default_n);

// 在调用线程中阻塞运行 HTTP 监控服务:epoll Reactor + timerfd 空闲连接回收 + HTTP 解析。
// roDb 须为只读连接,与主链路的写连接配合,依靠 SQLite 的 WAL 模式实现读写并发。
// 通常放在独立 std::thread 中运行,直到 should_stop 返回 true 才返回。
//
// 三个入口的形态差异是有意的,它把「这个值什么时候会变」编码进了签名:
//   port        —— C 档,进程生命周期内不变,按值传一次即可;
//   config      —— A 档,可在运行期改,故传取值器而非取好的值;
//   should_stop —— 跨线程的停机意愿,同样是取值器。本函数在自己的 1 秒
//                  timerfd 回调里轮询它,为 true 就退出循环 —— 于是调用方
//                  可以 join 这条线程,而不必 detach 后听天由命。
//                  代价是停机最多晚一个扫描周期(约 1 秒),换来的是进程能走完
//                  析构:异步日志缓冲得以刷出,这正是停机日志此前会丢的原因。
//                  传空表示永不停机(仅用于不关心退出的场合)。
void runHttpServer(Database& roDb, int port, HttpRuntimeConfigProvider config,
                   std::function<bool()> should_stop);

}  // namespace gateway
