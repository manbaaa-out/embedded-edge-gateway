/** @file 下行命令的无状态翻译与参数校验实现。 */

#include "gateway/cloud/CommandTranslator.h"

#include <cstdlib>

namespace gateway {

namespace {

/**
 * 构造失败结果。
 * @param why 拒绝原因。
 * @return ok=false 且带错误文本的结果。
 */
TranslateResult fail(std::string why) {
    TranslateResult r;  // 默认 ok=false，只填充错误说明。
    r.error = std::move(why);
    return r;
}

/**
 * 构造成功结果。
 * @param type 设备协议命令类型。
 * @param arg 不含序列号的参数字节。
 * @return ok=true 且带完整 DownCmd 的结果。
 */
TranslateResult ok(uint8_t type, std::vector<uint8_t> arg = {}) {
    TranslateResult r;  // 成功分支同时设置状态和完整命令。
    r.ok  = true;
    r.cmd = DownCmd{type, std::move(arg)};
    return r;
}

/**
 * 严格解析十进制整数。消息两端可带空白，有效内容不得带非数字后缀。
 *
 * @param raw MQTT payload 原文。
 * @param out 解析成功后写入的整数。
 * @return 是否完整解析出一个整数。
 */
bool parseStrictInt(const std::string& raw, long& out) {
    const auto begin = raw.find_first_not_of(" \t\r\n");  // 有效内容起点。
    if (begin == std::string::npos) return false;
    const auto end_pos = raw.find_last_not_of(" \t\r\n");  // 有效内容终点。
    const std::string s = raw.substr(begin, end_pos - begin + 1);  // 去掉两端空白。

    char*      end = nullptr;  // strtol 停止解析的位置。
    const long v   = std::strtol(s.c_str(), &end, 10);  // 待校验的解析值。
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

}  // namespace

TranslateResult translateCommand(const std::string& topic, const std::string& payload) {
    std::string      name = topic;  // 最终用于分派的命令名。
    const std::size_t pos = topic.find_last_of('/');  // 主题最后一级的分隔位置。
    if (pos != std::string::npos) name = topic.substr(pos + 1);

    if (name == "query_light") {
        return ok(EDGE_TYPE_QUERY_LIGHT);
    }
    if (name == "query_th") {
        return ok(EDGE_TYPE_QUERY_TH);
    }
    if (name == "set_period") {
        long period = 0;  // 设备采集周期，单位为秒。
        if (!parseStrictInt(payload, period)) {
            return fail("set_period 参数不是合法整数: '" + payload + "'");
        }
        if (period < static_cast<long>(EDGE_PERIOD_MIN_S) ||
            period > static_cast<long>(EDGE_PERIOD_MAX_S)) {
            return fail("set_period 超范围: " + payload + " (允许 " +
                        std::to_string(EDGE_PERIOD_MIN_S) + ".." +
                        std::to_string(EDGE_PERIOD_MAX_S) + " 秒)");
        }
        std::vector<uint8_t> arg(2);  // 协议规定的 uint16 大端周期字段。
        edge_u16_be_write(arg.data(), static_cast<uint16_t>(period));
        return ok(EDGE_TYPE_SET_PERIOD, std::move(arg));
    }

    return fail("未知命令: '" + name + "'");
}

}  // namespace gateway
