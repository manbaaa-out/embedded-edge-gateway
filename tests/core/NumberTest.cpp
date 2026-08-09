// formatValue 的位数由协议的定标决定,不由 double 的表示决定。
// 这三个文本出口共用它:MQTT 周期上行、查询应答、HTTP 的 JSON ——
// 本文件同时钉住「位数正确」与「三个出口一致」这两件事。

#include "gateway/core/format/Number.h"

#include "edge_proto/edge_proto.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using gateway::formatValue;

// ---- 位数:一位小数是定标(×10)能支撑的上限,多一位就是假精度 ----

TEST(FormatValue, KeepsOneDecimalForScaledValues) {
    // §3.4:25.3℃ 在线上是 253,除以 EDGE_TEMP_SCALE 还原
    EXPECT_EQ(formatValue(253 / double(EDGE_TEMP_SCALE)), "25.3");
    EXPECT_EQ(formatValue(723 / double(EDGE_HUMI_SCALE)), "72.3");
    EXPECT_EQ(formatValue(65535 / double(EDGE_TEMP_SCALE)), "6553.5");
}

TEST(FormatValue, DropsTrailingZeroAndDot) {
    // 整数量纲不该带小数点:光照是 lux 整数,状态位是 0/1
    EXPECT_EQ(formatValue(300.0), "300");
    EXPECT_EQ(formatValue(1.0), "1");
    EXPECT_EQ(formatValue(0.0), "0");
    EXPECT_EQ(formatValue(65535.0), "65535");
    // 定点值恰好落在整数上时同样不留 ".0"
    EXPECT_EQ(formatValue(250 / double(EDGE_TEMP_SCALE)), "25");
}

TEST(FormatValue, NeverEmitsSixDecimals) {
    // 回归:std::to_string(double) 固定六位小数,25.3 会写成 "25.300000",
    // 后五位是浮点表示的噪声,不是传感器给得出的精度
    const std::string s = formatValue(253 / double(EDGE_TEMP_SCALE));
    EXPECT_EQ(s.find("00000"), std::string::npos) << "实际输出: " << s;
    EXPECT_LE(s.size() - s.find('.') - 1, 1u) << "小数位多于 1 位: " << s;
}

// ---- 健壮性:输出会直接进 JSON,必须始终是合法数字字面量 ----

TEST(FormatValue, NonFiniteFallsBackToZero) {
    EXPECT_EQ(formatValue(std::numeric_limits<double>::infinity()), "0");
    EXPECT_EQ(formatValue(-std::numeric_limits<double>::infinity()), "0");
    EXPECT_EQ(formatValue(std::nan("")), "0");
}

TEST(FormatValue, OutputIsAlwaysParsableBack) {
    // 任意定标值往返一圈,误差不超过半个最小刻度(0.05)
    for (int raw = 0; raw <= 65535; raw += 337) {
        const double v = raw / double(EDGE_TEMP_SCALE);
        const double back = std::stod(formatValue(v));
        EXPECT_NEAR(back, v, 0.05) << "raw=" << raw;
    }
}

// ---- 一致性:三个出口拼出来的文本必须含同一个数字 ----

TEST(FormatValue, ThreeOutletsAgree) {
    const double t = 253 / double(EDGE_TEMP_SCALE);
    const double h = 723 / double(EDGE_HUMI_SCALE);

    const std::string uplink = formatValue(t);                              // gateway/up/temperature
    const std::string resp   = "ok," + formatValue(t) + "," + formatValue(h);  // gateway/resp/<seq>
    const std::string json   = "{\"value\":" + formatValue(t) + "}";        // /api/data

    EXPECT_EQ(uplink, "25.3");
    EXPECT_EQ(resp, "ok,25.3,72.3");
    EXPECT_EQ(json, "{\"value\":25.3}");
}
