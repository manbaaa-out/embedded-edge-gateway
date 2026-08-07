#include "gateway/cloud/CommandTranslator.h"

#include <cstdlib>

namespace gateway {

namespace {

TranslateResult fail(std::string why) {
    TranslateResult r;
    r.error = std::move(why);
    return r;
}

TranslateResult ok(uint8_t type, std::vector<uint8_t> arg = {}) {
    TranslateResult r;
    r.ok  = true;
    r.cmd = DownCmd{type, std::move(arg)};
    return r;
}

// 先 trim 两端空白,再要求剩余部分全部为数字。
//
// 两侧的严格程度都有理由:MQTT payload 常来自 shell,尾随换行是常态,故必须 trim;
// 而 trim 之后必须严格 —— std::stoi 会把 "12abc" 解析成 12,该参数将写入设备的
// 采样周期,不应容忍。
bool parseStrictInt(const std::string& raw, long& out) {
    const auto begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return false;   // 全是空白 / 空串
    const auto end_pos = raw.find_last_not_of(" \t\r\n");
    const std::string s = raw.substr(begin, end_pos - begin + 1);

    char*      end = nullptr;
    const long v   = std::strtol(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return false;   // 存在非数字尾部
    out = v;
    return true;
}

}  // namespace

TranslateResult translateCommand(const std::string& topic, const std::string& payload) {
    // gateway/cmd/set_period → set_period
    std::string      name = topic;
    const std::size_t pos = topic.find_last_of('/');
    if (pos != std::string::npos) name = topic.substr(pos + 1);

    if (name == "query_light") {
        return ok(EDGE_TYPE_QUERY_LIGHT);            // 无参数
    }
    if (name == "query_th") {
        return ok(EDGE_TYPE_QUERY_TH);               // 无参数
    }
    if (name == "set_period") {
        // payload 为周期秒数(§6.3)
        long period = 0;
        if (!parseStrictInt(payload, period)) {
            return fail("set_period 参数不是合法整数: '" + payload + "'");
        }
        if (period < static_cast<long>(EDGE_PERIOD_MIN_S) ||
            period > static_cast<long>(EDGE_PERIOD_MAX_S)) {
            return fail("set_period 超范围: " + payload + " (允许 " +
                        std::to_string(EDGE_PERIOD_MIN_S) + ".." +
                        std::to_string(EDGE_PERIOD_MAX_S) + " 秒)");
        }
        std::vector<uint8_t> arg(2);
        edge_u16_be_write(arg.data(), static_cast<uint16_t>(period));   // 大端,与节点一致
        return ok(EDGE_TYPE_SET_PERIOD, std::move(arg));
    }

    return fail("未知命令: '" + name + "'");
}

}  // namespace gateway
