// 遥测解码测试。decodeTelemetry 是纯函数,可脱离串口与数据库单独验证。

#include "gateway/pipeline/TelemetryDecoder.h"

#include <gtest/gtest.h>

using namespace gateway;

namespace {
Frame frame(uint8_t type, std::vector<uint8_t> payload) {
    return Frame{type, std::move(payload)};
}
}  // namespace

// 温湿度帧拆成两条记录,数值是定点值 ÷10(§3.4:25.3℃ → 253)
TEST(TelemetryDecoder, Dht11SplitsIntoTemperatureAndHumidity) {
    const auto r = decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD, 0x02, 0x5D, 0x00}));

    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].device, "temperature");
    EXPECT_DOUBLE_EQ(r[0].value, 25.3);
    EXPECT_EQ(r[1].device, "humidity");
    EXPECT_DOUBLE_EQ(r[1].value, 60.5);
}

TEST(TelemetryDecoder, Bh1750DecodesBigEndianLux) {
    const auto r = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01, 0x90}));

    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].device, "illuminance");
    EXPECT_DOUBLE_EQ(r[0].value, 400.0) << "大端读反会得到 0x9001 = 36865";
}

// 状态帧是按位标志,必须逐位 AND,不可把整字节当作单一数值。
// 四种组合全部覆盖,以排除「把 0x03 当成 3 号状态」这类误解。
TEST(TelemetryDecoder, StatusBitmaskSplitsIntoTwoHealthReadings) {
    struct Case {
        uint8_t mask;
        double  dht11;
        double  bh1750;
        const char* desc;
    };
    const Case cases[] = {
        {0x00, 0.0, 0.0, "两路都故障"},
        {0x01, 1.0, 0.0, "只有温湿度在线"},
        {0x02, 0.0, 1.0, "只有光照在线"},
        {0x03, 1.0, 1.0, "两路都在线"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.desc);
        const auto r = decodeTelemetry(frame(EDGE_TYPE_STATUS, {c.mask}));

        ASSERT_EQ(r.size(), 2u);
        EXPECT_EQ(r[0].device, "status_dht11");
        EXPECT_DOUBLE_EQ(r[0].value, c.dht11);
        EXPECT_EQ(r[1].device, "status_bh1750");
        EXPECT_DOUBLE_EQ(r[1].value, c.bh1750);
    }
}

// 心跳仅表明节点存活,不应落库,否则会产生大量无数值的记录
TEST(TelemetryDecoder, HeartbeatProducesNothing) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_HEARTBEAT, {})).empty());
}

// payload 短于该 TYPE 的要求时必须丢弃,不得越界读取并落库。
// 长度口径统一取自 edge_min_payload_len。
TEST(TelemetryDecoder, ShortPayloadIsDroppedNotMisread) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD})).empty())
        << "DHT11 要 5 字节,给 2 字节必须丢";
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_STATUS, {})).empty());
}

TEST(TelemetryDecoder, UnknownTypeIsDropped) {
    EXPECT_TRUE(decodeTelemetry(frame(0x99, {0x11, 0x22})).empty());
}

// 应答帧属命令链路,应由调用方先行分流;此处兜底丢弃,
// 避免 ACK 的 seq 被当作传感器数值落库。
TEST(TelemetryDecoder, AckFramesAreNotTelemetry) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_ACK, {0x09, 0x00})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_QUERY_RESP, {0x07, 0x00, 0x01, 0x90})).empty());
}

// 超出最小长度的多余字节应被忽略而非报错,为协议向后扩展留出空间
TEST(TelemetryDecoder, ExtraPayloadBytesAreIgnored) {
    const auto r = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01, 0x90, 0xFF, 0xFF}));

    ASSERT_EQ(r.size(), 1u);
    EXPECT_DOUBLE_EQ(r[0].value, 400.0);
}

// 边界值:0 与 uint16 上限均应如实解出
TEST(TelemetryDecoder, HandlesExtremeValues) {
    const auto zero = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x00, 0x00}));
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_DOUBLE_EQ(zero[0].value, 0.0);

    const auto max = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0xFF, 0xFF}));
    ASSERT_EQ(max.size(), 1u);
    EXPECT_DOUBLE_EQ(max[0].value, 65535.0);
}
