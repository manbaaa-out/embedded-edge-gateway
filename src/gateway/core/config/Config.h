#pragma once
#include <string>
#include <memory>

namespace gateway {

// /api/data 单次返回的点数上限,同时也是 report_n 这个字段的取值上限。
//
// 定义在这里而不是 HttpServer.h,是因为「一个字段的合法范围」应当和字段本身
// 待在一起:上限若只存在于 io 层,配置侧就没有上限可查,运维写 report_n = 5000
// 得不到任何提示,值被静默夹成 1000 —— 静默改写用户给的值,比拒绝它更糟。
// io 层的 clampReportN 仍然必须夹紧,那是另一件事:它同时处理 URL 上不可信的
// ?n= 参数。两个入口共用同一个上限,但只有一处定义。
inline constexpr int kMaxReportN = 1000;

struct Config {
    // A 档
    int log_level      = 1;
    int idle_timeout   = 5;
    int report_n       = 10;   // 合法范围 [1, kMaxReportN]
    int mqtt_keepalive = 60;
    // B 档
    std::string serial_path = "/tmp/ttyV0";
    int         serial_baud = 115200;
    std::string mqtt_host   = "localhost";
    std::string db_path     = "/tmp/gateway.db";
    // C 档(运行期不可变)
    int mqtt_port    = 1883;
    int http_port    = 8888;
};

class ConfigManager {
public:
    static void init(const std::string& path);            // 启动时调用一次,失败抛异常
    static std::shared_ptr<const Config> current();       // 可在任意线程调用,返回快照

    struct ReloadResult {
        bool ok             = false;
        bool serial_changed = false;
        bool mqtt_changed   = false;
        bool db_changed     = false;
    };
    static ReloadResult reload();                          // 仅由主循环调用

private:
    static Config parseFile(const std::string& path);     // 解析失败抛异常
    static bool   validate(const Config& c);

    static std::string                   path_;
    static std::shared_ptr<const Config> current_;
    static std::shared_ptr<const Config> startup_;
};

} // namespace gateway