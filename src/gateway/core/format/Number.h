#pragma once

// 遥测数值的统一文本格式。
//
// 线上温湿度只有 0.1 分辨率，光照和状态为整数，因此固定保留一位后再去掉尾零即可
// 表达协议提供的全部精度。MQTT、命令响应和 HTTP 共用本函数，避免相同读数在不同
// 出口出现不同字面量。非有限值和 snprintf 失败降级为合法 JSON 数字 "0"；构造返回
// 字符串所需的内存分配仍可能抛出 std::bad_alloc。

#include <cmath>
#include <cstdio>
#include <string>

namespace gateway {

/**
 * @brief 将遥测值格式化为最多一位小数的十进制文本。
 * @param v 待格式化数值；正常来源为协议中 uint16_t 解码得到的有限小数。
 * @return 去除小数尾零后的数字字符串；非有限值或 snprintf 失败时返回 "0"。
 *
 * 内部使用 32 字节固定缓冲区；超出该固定十进制表示范围的非常规有限输入会被截断。
 */
inline std::string formatValue(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }

    char buf[32]; // 保存 snprintf 写入的、以 NUL 结尾的固定小数文本。
    const int n = std::snprintf(buf, sizeof(buf), "%.1f", v); // 完整结果所需字符数。
    if (n < 0) {
        return "0";
    }
    // len 是缓冲区中实际可用的字符数，不包含终止 NUL。
    const std::size_t len =
        (static_cast<std::size_t>(n) < sizeof(buf)) ? static_cast<std::size_t>(n) : sizeof(buf) - 1;
    std::string s(buf, len); // 后续原地移除尾零的可变结果。

    const std::size_t dot = s.find('.'); // 小数点位置；截断的超大输入可能找不到。
    if (dot == std::string::npos) {
        return s;
    }
    std::size_t last = s.find_last_not_of('0'); // 需要保留的最后一个非零字符。
    if (last == dot) {
        last = dot - 1;
    }
    s.erase(last + 1);
    return s;
}

}  // namespace gateway
