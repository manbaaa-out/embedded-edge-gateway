// decodeTelemetry 的纯函数契约测试：输入已通过链路 CRC 的 Frame，输出零到多条带
// 设备名和物理量的 Reading。重点覆盖协议定标、长度防护、类型分流和扩展兼容。

#include "gateway/pipeline/TelemetryDecoder.h"

#include <gtest/gtest.h>

using namespace gateway;

namespace {
// 构造仅包含 type 和 payload 的测试 Frame；payload 按值接收后移动，便于列表初始化。
Frame frame(uint8_t type, std::vector<uint8_t> payload) {
    return Frame{type, std::move(payload)};
}
} // namespace

// 温湿度共用一帧，两个大端定标值分别生成独立读数。
TEST(TelemetryDecoder, Dht11SplitsIntoTemperatureAndHumidity) {
    const auto r =
        decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD, 0x02, 0x5D})); // 两条解码读数。

    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].device, "temperature");
    EXPECT_DOUBLE_EQ(r[0].value, 25.3);
    EXPECT_EQ(r[1].device, "humidity");
    EXPECT_DOUBLE_EQ(r[1].value, 60.5);
}

// BH1750 的两字节照度采用大端序，0x0190 应还原为 400 lux。
TEST(TelemetryDecoder, Bh1750DecodesBigEndianLux) {
    const auto r = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01, 0x90})); // 单条照度读数。

    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].device, "illuminance");
    EXPECT_DOUBLE_EQ(r[0].value, 400.0) << "大端读反会得到 0x9001 = 36865";
}

// 覆盖两位状态掩码的全部组合，确认每个传感器独立解码。
TEST(TelemetryDecoder, StatusBitmaskSplitsIntoTwoHealthReadings) {
    struct Case {
        uint8_t mask;     // 输入状态位掩码。
        double dht11;     // 期望的温湿度传感器健康值。
        double bh1750;    // 期望的光照传感器健康值。
        const char* desc; // SCOPED_TRACE 的组合描述。
    };
    const Case cases[] = {
        {0x00, 0.0, 0.0, "两路都故障"},
        {0x01, 1.0, 0.0, "只有温湿度在线"},
        {0x02, 0.0, 1.0, "只有光照在线"},
        {0x03, 1.0, 1.0, "两路都在线"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.desc);
        const auto r =
            decodeTelemetry(frame(EDGE_TYPE_STATUS, {c.mask})); // 当前掩码拆出的两条状态。

        ASSERT_EQ(r.size(), 2u);
        EXPECT_EQ(r[0].device, "status_dht11");
        EXPECT_DOUBLE_EQ(r[0].value, c.dht11);
        EXPECT_EQ(r[1].device, "status_bh1750");
        EXPECT_DOUBLE_EQ(r[1].value, c.bh1750);
    }
}

// 心跳没有测量值，不生成存储记录。
TEST(TelemetryDecoder, HeartbeatProducesNothing) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_HEARTBEAT, {})).empty());
}

// 负载短于类型契约时丢弃整帧，避免越界读取残缺数值。
TEST(TelemetryDecoder, ShortPayloadIsDroppedNotMisread) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD})).empty())
        << "DHT11 要 4 字节,给 2 字节必须丢";
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_STATUS, {})).empty());
}

// 业务字典之外的 TYPE 不产生通用或占位读数，交由调用方记录诊断。
TEST(TelemetryDecoder, UnknownTypeIsDropped) {
    EXPECT_TRUE(decodeTelemetry(frame(0x99, {0x11, 0x22})).empty());
}

// 命令应答不属于遥测；解码器再次防止其序号和状态被落库。
TEST(TelemetryDecoder, AckFramesAreNotTelemetry) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_ACK, {0x09, 0x00})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_QUERY_RESP, {0x07, 0x00, 0x01, 0x90})).empty());
}

// 已知前缀足够解码时忽略尾随扩展字段。
TEST(TelemetryDecoder, ExtraPayloadBytesAreIgnored) {
    const auto r =
        decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01, 0x90, 0xFF, 0xFF})); // 带未知尾随字段。

    ASSERT_EQ(r.size(), 1u);
    EXPECT_DOUBLE_EQ(r[0].value, 400.0);
}

// 旧版 DHT11 帧的尾随校验字节按扩展字段忽略，支持滚动升级。
TEST(TelemetryDecoder, LegacyDht11FrameWithTrailingChecksumStillDecodes) {
    const auto r =
        decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD, 0x02, 0x5D, 0x5C})); // 旧版五字节负载。

    ASSERT_EQ(r.size(), 2u);
    EXPECT_DOUBLE_EQ(r[0].value, 25.3);
    EXPECT_DOUBLE_EQ(r[1].value, 60.5);
}

// 无符号两字节量的两个端点均应无损解码。
TEST(TelemetryDecoder, HandlesExtremeValues) {
    const auto zero = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x00, 0x00})); // uint16_t 下界。
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_DOUBLE_EQ(zero[0].value, 0.0);

    const auto max = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0xFF, 0xFF})); // uint16_t 上界。
    ASSERT_EQ(max.size(), 1u);
    EXPECT_DOUBLE_EQ(max[0].value, 65535.0);
}
