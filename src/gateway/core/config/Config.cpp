#include "gateway/core/config/Config.h"
#include "gateway/core/log/Logger.h"
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cassert>

namespace gateway {

// ---- 静态成员定义 ----
std::string                   ConfigManager::path_;
std::shared_ptr<const Config> ConfigManager::current_;
std::shared_ptr<const Config> ConfigManager::startup_;

// ---- 解析辅助 ----
static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static int toInt(const std::string& val, const std::string& key) {
    try {
        size_t pos = 0;
        int v = std::stoi(val, &pos);
        if (pos != val.size()) throw std::invalid_argument("trailing chars");
        return v;
    } catch (...) {
        throw std::runtime_error("config '" + key + "' invalid int value: '" + val + "'");
    }
}

Config ConfigManager::parseFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    Config c;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            LOG_WARN("config line %d: no '=', skipped: %s", lineno, t.c_str());
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));

        // 去除 value 中的行尾注释:第一个 '#' 及其之后全部丢弃
        size_t hash = val.find('#');
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
    bool ok = true;
    auto checkPort = [&](const char* name, int p) {
        if (p < 1 || p > 65535) {
            LOG_WARN("config %s invalid port: %d (1..65535)", name, p); ok = false;
        }
    };
    checkPort("mqtt_port", c.mqtt_port);
    checkPort("http_port", c.http_port);
    if (c.idle_timeout   <= 0) { LOG_WARN("idle_timeout invalid: %d (>0)", c.idle_timeout); ok = false; }
    if (c.report_n       <= 0) { LOG_WARN("report_n invalid: %d (>0)", c.report_n); ok = false; }
    if (c.mqtt_keepalive <  0) { LOG_WARN("mqtt_keepalive invalid: %d (>=0)", c.mqtt_keepalive); ok = false; }
    if (c.serial_baud    <= 0) { LOG_WARN("serial_baud invalid: %d", c.serial_baud); ok = false; }
    auto checkPath = [&](const char* name, const std::string& p) {
        if (p.empty()) { LOG_WARN("config %s must not be empty", name); ok = false; }
    };
    checkPath("serial_path", c.serial_path);
    checkPath("db_path",     c.db_path);
    checkPath("mqtt_host",   c.mqtt_host);
    return ok;
}

void ConfigManager::init(const std::string& path) {
    auto cfg = std::make_shared<Config>(parseFile(path));   // 解析失败的异常传给调用方
    path_    = path;
    startup_ = std::make_shared<Config>(*cfg);
    std::atomic_store(&current_, std::shared_ptr<const Config>(cfg));
}

std::shared_ptr<const Config> ConfigManager::current() {
    return std::atomic_load(&current_);
}

ConfigManager::ReloadResult ConfigManager::reload() {
    ReloadResult result;
    try {
        auto fresh = std::make_shared<Config>(parseFile(path_));
        if (!validate(*fresh)) return result;             // 校验失败,旧配置保持不变

        auto restoreC = [](const char* name, int& fv, int sv) {
            if (fv != sv)
                LOG_WARN("config '%s' needs restart (current=%d, file's %d takes effect after restart)",
                         name, sv, fv);
            fv = sv;
        };
        restoreC("mqtt_port",    fresh->mqtt_port,    startup_->mqtt_port);
        restoreC("http_port",    fresh->http_port,    startup_->http_port);

        auto old = std::atomic_load(&current_);
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