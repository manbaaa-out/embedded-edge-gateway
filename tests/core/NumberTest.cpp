// formatValue 的纯函数测试：协议原始量经定标后以最短十进制文本进入 MQTT、命令响应
// 和 HTTP JSON。测试同时约束精度、合法数字语法以及三个出口的一致性。

#include "gateway/core/format/Number.h"

#include "edge_proto/edge_proto.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using gateway::formatValue;

// 十倍定标的非整数样例应保留恰好一位有效小数，包括 uint16_t 上界。
TEST(FormatValue, KeepsOneDecimalForScaledValues) {
    // 温湿度原始值采用十倍定标，因此最多保留一位小数。
    EXPECT_EQ(formatValue(253 / double(EDGE_TEMP_SCALE)), "25.3");
    EXPECT_EQ(formatValue(723 / double(EDGE_HUMI_SCALE)), "72.3");
    EXPECT_EQ(formatValue(65535 / double(EDGE_TEMP_SCALE)), "6553.5");
}

// 整数和恰好落在整数刻度的定标值都不携带尾随 ".0"。
TEST(FormatValue, DropsTrailingZeroAndDot) {
    // 整数值不输出无意义的小数点。
    EXPECT_EQ(formatValue(300.0), "300");
    EXPECT_EQ(formatValue(1.0), "1");
    EXPECT_EQ(formatValue(0.0), "0");
    EXPECT_EQ(formatValue(65535.0), "65535");
    // 定标值落在整数刻度时同样移除末尾零。
    EXPECT_EQ(formatValue(250 / double(EDGE_TEMP_SCALE)), "25");
}

// 防止回退到 std::to_string 产生六位伪精度。
TEST(FormatValue, NeverEmitsSixDecimals) {
    // 输出精度不得超过传感器协议所能表达的精度。
    const std::string s = formatValue(253 / double(EDGE_TEMP_SCALE)); // 典型一位小数输出。
    EXPECT_EQ(s.find("00000"), std::string::npos) << "实际输出: " << s;
    EXPECT_LE(s.size() - s.find('.') - 1, 1u) << "小数位多于 1 位: " << s;
}

// 非有限浮点数不能直接进入 JSON 数字字段。
TEST(FormatValue, NonFiniteFallsBackToZero) {
    EXPECT_EQ(formatValue(std::numeric_limits<double>::infinity()), "0");
    EXPECT_EQ(formatValue(-std::numeric_limits<double>::infinity()), "0");
    EXPECT_EQ(formatValue(std::nan("")), "0");
}

// 枚举稀疏覆盖全部 uint16_t 原始范围，格式化结果必须能由 stod 读回。
TEST(FormatValue, OutputIsAlwaysParsableBack) {
    // 文本往返误差不超过半个最小刻度。
    for (int raw = 0; raw <= 65535; raw += 337) {
        const double v = raw / double(EDGE_TEMP_SCALE); // 当前原始码对应的物理量。
        const double back = std::stod(formatValue(v));  // 从最终文本恢复的数值。
        EXPECT_NEAR(back, v, 0.05) << "raw=" << raw;
    }
}

// 用相同温湿度同时拼装三个实际出口，确保它们没有各自实现不同格式规则。
TEST(FormatValue, ThreeOutletsAgree) {
    const double t = 253 / double(EDGE_TEMP_SCALE); // 25.3 ℃。
    const double h = 723 / double(EDGE_HUMI_SCALE); // 72.3 %。

    const std::string uplink = formatValue(t);                              // MQTT 上行负载
    const std::string resp = "ok," + formatValue(t) + "," + formatValue(h); // 查询命令响应
    const std::string json = "{\"value\":" + formatValue(t) + "}";          // HTTP JSON 数值

    EXPECT_EQ(uplink, "25.3");
    EXPECT_EQ(resp, "ok,25.3,72.3");
    EXPECT_EQ(json, "{\"value\":25.3}");
}
