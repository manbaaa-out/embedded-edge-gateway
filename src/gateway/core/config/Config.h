#pragma once
#include <string>
#include <memory>

namespace gateway {

struct Config {
    // A 档
    int log_level      = 1;
    int idle_timeout   = 5;
    int report_n       = 10;
    int mqtt_keepalive = 60;
    // B 档
    std::string serial_path = "/tmp/ttyV0";
    int         serial_baud = 115200;
    std::string mqtt_host   = "localhost";
    std::string db_path     = "/tmp/gateway.db";
    // C 档(运行期不可变)
    int mqtt_port    = 1883;
    int http_port    = 8888;
    int worker_count = 4;
};

class ConfigManager {
public:
    static void init(const std::string& path);            // 启动调用一次,失败抛异常
    static std::shared_ptr<const Config> current();       // 任意线程读,拿快照

    struct ReloadResult {
        bool ok             = false;
        bool serial_changed = false;
        bool mqtt_changed   = false;
        bool db_changed     = false;
    };
    static ReloadResult reload();                          // 主循环调用

private:
    static Config parseFile(const std::string& path);     // 解析失败抛异常
    static bool   validate(const Config& c);

    static std::string                   path_;
    static std::shared_ptr<const Config> current_;
    static std::shared_ptr<const Config> startup_;
};

} // namespace gateway