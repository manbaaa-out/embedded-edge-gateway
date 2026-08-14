#pragma once

// 配置的解析、校验与热加载。SIGHUP 触发 reload,读侧无锁。
//
// 配置项按「改了之后要付多大代价才能生效」分三档,这个分法决定了热加载做什么:
//   A 改内存即生效  —— 没有资源依附于它,每次用时现读
//   B 需重建资源    —— 串口/连接是照着旧值建起来的,得摘掉重来
//   C 运行期不可变  —— 值已固化进 bind 好的 socket,只能压回启动值并告警

#include <string>
#include <memory>

namespace gateway {

// report_n 的取值上限,同时也是 /api/data 单次返回的点数上限。
//
// 放在这里而不是 HttpServer.h:一个字段的合法范围该和字段待在一起。只放 io 层的话
// 配置侧就没有上限可查,写 report_n = 5000 得不到提示、值被静默夹成 1000 ——
// 静默改写用户给的值比拒绝它更糟。io 层的 clampReportN 仍要夹,那是另一个入口
// (URL 上不可信的 ?n=)。两个入口共用上限,但只有一处定义。
inline constexpr int kMaxReportN = 1000;

struct Config {
    // A 档:改内存即生效
    int log_level      = 1;
    int idle_timeout   = 5;
    int report_n       = 10;   // 合法范围 [1, kMaxReportN]
    // B 档:需重建对应资源
    std::string serial_path = "/tmp/ttyV0";
    int         serial_baud = 115200;
    std::string mqtt_host   = "localhost";
    std::string db_path     = "/tmp/gateway.db";
    // keepalive 属 B 档而非 A 档:它被写进 CONNECT 报文,改内存不会生效,必须重连。
    // reload() 一直是按 B 档处理的(它进 mqtt_changed 的 diff),此前只是错挂在
    // A 档的注释下 —— 照那个分档去理解,会以为改它是零代价的,实际上行会短暂中断。
    int mqtt_keepalive = 60;
    // C 档:运行期不可变
    int mqtt_port    = 1883;
    int http_port    = 8888;
};

// 读侧无锁靠的是 load-then-swap:新配置整份建好、校验通过,最后一行原子换指针。
// 读者拿到的永远是某一版【完整】配置,不会看到改了一半的中间态;
// 失败时旧配置从未被触碰,所以也不需要回滚。
class ConfigManager {
public:
    static void init(const std::string& path);            // 启动时一次,失败抛异常
    static std::shared_ptr<const Config> current();       // 任意线程可调,返回快照

    // 哪几类资源需要重建,由调用方按位处理。diff 必须精确:
    // 多报会让串口与连接被无谓重建,少报则修改不生效。
    struct ReloadResult {
        bool ok             = false;
        bool serial_changed = false;
        bool mqtt_changed   = false;
        bool db_changed     = false;
    };
    static ReloadResult reload();                          // 仅由主循环调用

private:
    static Config parseFile(const std::string& path);     // 解析失败抛异常
    static bool   validate(const Config& c);              // 纯函数,不碰 I/O

    static std::string                   path_;
    static std::shared_ptr<const Config> current_;
    static std::shared_ptr<const Config> startup_;        // C 档的锚
};

} // namespace gateway