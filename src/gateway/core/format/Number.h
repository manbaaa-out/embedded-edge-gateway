#pragma once

// 遥测数值的统一文本格式。
//
// 同一个读数有三个文本出口:MQTT 周期上行(gateway/up/*)、查询应答(gateway/resp/*)、
// HTTP 的 JSON。三者必须用同一份实现,否则同一个值在不同出口长得不一样,对不上账。
//
// 位数取自协议而非取自 double:payload 里温湿度是 ×10 定点(EDGE_TEMP_SCALE),
// 最细分辨率就是 0.1;光照与状态位是整数量纲。所以一位小数已经是数据能支撑的上限,
// 多一位都是传感器给不出的精度 —— std::to_string(double) 固定六位小数,
// 25.3 会写成 "25.300000",看着像六位有效数字,实际后五位全是浮点表示的噪声。
//
// 末尾多余的 0 连同小数点一并去掉,让整数量纲保持整数形态(300 lux 写成 "300"
// 而不是 "300.0",状态位写成 "1" 而不是 "1.0")。

#include <cmath>
#include <cstdio>
#include <string>

namespace gateway {

inline std::string formatValue(double v) {
    // 本协议的数值都由 uint16 除以定标常数得来,不可能是 inf/nan;
    // 但这个函数的输出会直接进 JSON,而 "inf"/"nan" 不是合法的 JSON 数字,
    // 故在此把它挡掉,让函数对任意输入都返回一个合法数字字面量。
    if (!std::isfinite(v)) {
        return "0";
    }

    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.1f", v);
    if (n < 0) {
        return "0";   // 格式化失败,给一个合法的数字字面量,别让 JSON 破格
    }
    // snprintf 返回的是「本应写入的长度」,可能超出缓冲区,故须夹紧
    const std::size_t len =
        (static_cast<std::size_t>(n) < sizeof(buf)) ? static_cast<std::size_t>(n) : sizeof(buf) - 1;
    std::string s(buf, len);

    const std::size_t dot = s.find('.');
    if (dot == std::string::npos) {
        return s;   // 值太大时 %.1f 可能退化成无小数点的形式
    }
    std::size_t last = s.find_last_not_of('0');   // '.' 本身不是 '0',故 last >= dot
    if (last == dot) {
        last = dot - 1;   // 小数部分全为 0,小数点也一并去掉
    }
    s.erase(last + 1);
    return s;
}

}  // namespace gateway
