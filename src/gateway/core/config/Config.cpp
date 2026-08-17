// key=value 配置文件的解析、校验和快照发布实现。
//
// 解析以 Config 默认值为基线，因此未出现的键沿用默认值，重复键以最后一行为准。
// 语法没有引号或转义，值中首个 # 开始的内容一律视为注释。无法识别的整行或键只告警，
// 但已知键的值必须完整解析且通过统一校验。启动和热加载共用同一条校验路径，区别仅在
// 失败策略：init() 向上抛出，reload() 捕获后保留旧快照。

#include "gateway/core/config/Config.h"
#include "gateway/core/log/Logger.h"
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cassert>

namespace gateway {

std::string                   ConfigManager::path_;
std::shared_ptr<const Config> ConfigManager::current_;
std::shared_ptr<const Config> ConfigManager::startup_;

/**
 * @param s 待处理原字符串。
 * @return 去除两端空格、制表、回车和换行后的副本；全空白输入返回空串。
 */
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"); // 首个非空白字符位置。
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n"); // 末个非空白字符位置。
    return s.substr(b, e - b + 1);
}

/**
 * @param val 已去除首尾空白的整数文本。
 * @param key 对应配置键，仅用于错误消息。
 * @return 完整转换后的 int。
 * @throws std::runtime_error 无数字、越界或存在尾随字符。
 */
static int toInt(const std::string& val, const std::string& key) {
    try {
        size_t pos = 0;                  // std::stoi 停止解析后的字符偏移。
        int v = std::stoi(val, &pos);    // 已转换、尚待检查尾随字符的整数。
        if (pos != val.size()) throw std::invalid_argument("trailing chars");
        return v;
    } catch (...) {
        throw std::runtime_error("config '" + key + "' invalid int value: '" + val + "'");
    }
}

Config ConfigManager::parseFile(const std::string& path) {
    std::ifstream in(path); // 本次解析独占的输入流，函数返回时关闭。
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    Config c;          // 以结构体默认值为未配置字段的回退值。
    std::string line;  // 当前尚未规范化的文件行。
    int lineno = 0;    // 面向用户的 1-based 行号。
    while (std::getline(in, line)) {
        lineno++;
        std::string t = trim(line); // 去除首尾空白后的有效行。
        if (t.empty() || t[0] == '#') continue;

        size_t eq = t.find('='); // 第一个等号分隔键和值，值中后续等号原样保留。
        if (eq == std::string::npos) {
            LOG_WARN("config line %d: no '=', skipped: %s", lineno, t.c_str());
            continue;
        }
        std::string key = trim(t.substr(0, eq));     // 规范化后的配置键。
        std::string val = trim(t.substr(eq + 1));   // 尚未移除行尾注释的配置值。

        // 值中第一个 '#' 开始的内容视为行尾注释。
        size_t hash = val.find('#'); // 行尾注释起点；路径和主机名也遵循此语法。
        if (hash != std::string::npos) {
            val = trim(val.substr(0, hash));
        }

        if      (key == "log_level")      c.log_level      = toInt(val, key);
        else if (key == "idle_timeout")   c.idle_timeout   = toInt(val, key);
        else if (key == "report_n")       c.report_n       = toInt(val, key);
        else if (key == "mqtt_keepalive") c.mqtt_keepalive = toInt(val, key);
        else if (key == "serial_baud")    c.serial_baud    = toInt(val, key);
        else if (key == "mqtt_port")      c.mqtt_port      = toInt(val, key);
        else if (key == "http_port")      c.http_port      = toInt(val, key);
        else if (key == "serial_path")    c.serial_path    = val;
        else if (key == "mqtt_host")      c.mqtt_host      = val;
        else if (key == "db_path")        c.db_path        = val;
        else
            LOG_WARN("config line %d: unknown key, ignored: %s", lineno, key.c_str());
    }
    return c;
}

bool ConfigManager::validate(const Config& c) {
    bool ok = true; // 汇总全部字段错误，以便一次加载报告所有问题。
    // name 仅用于诊断，p 是待检查的 TCP 端口。
    auto checkPort = [&](const char* name, int p) {
        if (p < 1 || p > 65535) {
            LOG_WARN("config %s invalid port: %d (1..65535)", name, p); ok = false;
        }
    };
    checkPort("mqtt_port", c.mqtt_port);
    checkPort("http_port", c.http_port);
    if (c.idle_timeout   <= 0) { LOG_WARN("idle_timeout invalid: %d (>0)", c.idle_timeout); ok = false; }
    if (c.report_n <= 0 || c.report_n > kMaxReportN) {
        LOG_WARN("report_n invalid: %d (1..%d)", c.report_n, kMaxReportN); ok = false;
    }
    if (c.mqtt_keepalive <  0) { LOG_WARN("mqtt_keepalive invalid: %d (>=0)", c.mqtt_keepalive); ok = false; }
    if (c.serial_baud    <= 0) { LOG_WARN("serial_baud invalid: %d", c.serial_baud); ok = false; }
    // 路径和 mqtt_host 共用非空约束；name 用于生成可定位的日志。
    auto checkPath = [&](const char* name, const std::string& p) {
        if (p.empty()) { LOG_WARN("config %s must not be empty", name); ok = false; }
    };
    checkPath("serial_path", c.serial_path);
    checkPath("db_path",     c.db_path);
    checkPath("mqtt_host",   c.mqtt_host);
    return ok;
}

void ConfigManager::init(const std::string& path) {
    auto cfg = std::make_shared<Config>(parseFile(path)); // 校验前的候选启动快照。

    // 启动配置非法时直接失败，避免以部分默认值继续启动。
    if (!validate(*cfg))
        throw std::runtime_error("config validation failed: " + path +
                                 " (具体哪一项见上面的 WARN 日志)");

    path_    = path;
    startup_ = std::make_shared<Config>(*cfg);
    std::atomic_store(&current_, std::shared_ptr<const Config>(cfg));
}

std::shared_ptr<const Config> ConfigManager::current() {
    return std::atomic_load(&current_);
}

ConfigManager::ReloadResult ConfigManager::reload() {
    ReloadResult result; // 默认表示失败且没有可应用的资源差异。
    try {
        auto fresh = std::make_shared<Config>(parseFile(path_)); // 尚未发布的新候选快照。
        if (!validate(*fresh)) return result;

        // fv 是文件候选值，sv 是启动期实际值；不可热加载字段始终恢复为 sv。
        auto restoreC = [](const char* name, int& fv, int sv) {
            if (fv != sv)
                LOG_WARN("config '%s' needs restart (current=%d, file's %d takes effect after restart)",
                         name, sv, fv);
            fv = sv;
        };
        restoreC("mqtt_port",    fresh->mqtt_port,    startup_->mqtt_port);
        restoreC("http_port",    fresh->http_port,    startup_->http_port);

        auto old = std::atomic_load(&current_); // 比较期间保持旧快照存活。
        assert(old && "reload() called before init()");
        if (old->serial_path != fresh->serial_path ||
            old->serial_baud != fresh->serial_baud) result.serial_changed = true;
        if (old->mqtt_host      != fresh->mqtt_host ||
            old->mqtt_keepalive != fresh->mqtt_keepalive) result.mqtt_changed = true;
        if (old->db_path != fresh->db_path) result.db_changed = true;

        std::atomic_store(&current_, std::shared_ptr<const Config>(fresh));
        result.ok = true;
    } catch (const std::exception& e) {
        LOG_WARN("config reload failed, keep old config: %s", e.what());
    }
    return result;
}

} // namespace gateway
