// 遥测解码测试
//
// 重构前这段逻辑是 GatewayApp.cpp 匿名 namespace 里的 decodeFrame(),
// 想验证「0x04 的 bitmask 是否被正确拆成两路」得把整条链路跑起来。

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

// 状态帧是【按位标志】,必须逐位 AND —— 不能把整字节当单一数值。
// 四种组合全测,因为「把 0x03 当成第 3 号状态」这类误解正是本注释的由来。
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

// 心跳只证明节点活着,不该落库 —— 否则数据库里全是没有数值的噪声行
TEST(TelemetryDecoder, HeartbeatProducesNothing) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_HEARTBEAT, {})).empty());
}

// payload 短于该 TYPE 的要求时必须丢弃,绝不能越界读出垃圾数值落库。
// 长度口径统一走 edge_min_payload_len,不再各处手写魔数。
TEST(TelemetryDecoder, ShortPayloadIsDroppedNotMisread) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_DHT11, {0x00, 0xFD})).empty())
        << "DHT11 要 5 字节,给 2 字节必须丢";
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_STATUS, {})).empty());
}

TEST(TelemetryDecoder, UnknownTypeIsDropped) {
    EXPECT_TRUE(decodeTelemetry(frame(0x99, {0x11, 0x22})).empty());
}

// 应答帧走的是命令链路,不该混进遥测。调用方本该先分流,
// 这里兜底丢弃,免得 ACK 的 seq 被当成传感器数值落进库里。
TEST(TelemetryDecoder, AckFramesAreNotTelemetry) {
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_ACK, {0x09, 0x00})).empty());
    EXPECT_TRUE(decodeTelemetry(frame(EDGE_TYPE_QUERY_RESP, {0x07, 0x00, 0x01, 0x90})).empty());
}

// 超出 payload 最小长度的多余字节应被忽略而非报错 —— 为协议向后扩展留余地
TEST(TelemetryDecoder, ExtraPayloadBytesAreIgnored) {
    const auto r = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x01, 0x90, 0xFF, 0xFF}));

    ASSERT_EQ(r.size(), 1u);
    EXPECT_DOUBLE_EQ(r[0].value, 400.0);
}

// 边界值:0 与 uint16 上限都该如实解出
TEST(TelemetryDecoder, HandlesExtremeValues) {
    const auto zero = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0x00, 0x00}));
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_DOUBLE_EQ(zero[0].value, 0.0);

    const auto max = decodeTelemetry(frame(EDGE_TYPE_BH1750, {0xFF, 0xFF}));
    ASSERT_EQ(max.size(), 1u);
    EXPECT_DOUBLE_EQ(max[0].value, 65535.0);
}
