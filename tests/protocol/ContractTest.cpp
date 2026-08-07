// 协议契约的性质测试。
//
// 金标准向量验证具体的帧是否正确,本文件验证契约本身的性质:TYPE 分段是否不相交、
// 边界是否挡得住、错误路径是否能 resync。任一性质被破坏,两端就会以不同方式
// 解释同一串字节。

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

struct Capture {
    int                  frames = 0;
    uint8_t              type   = 0;
    std::vector<uint8_t> payload;
};

void onFrame(uint8_t type, const uint8_t* p, uint8_t len, void* user) {
    auto* c = static_cast<Capture*>(user);
    c->frames++;
    c->type    = type;
    c->payload.assign(p, p + len);
}

// 组帧 + 立刻解回来,返回解析器状态供断言
Capture feedFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    uint8_t buf[EDGE_FRAME_MAX];
    uint8_t n = edge_frame_encode(type, payload.empty() ? nullptr : payload.data(),
                                  static_cast<uint8_t>(payload.size()), buf);
    Capture       cap;
    edge_parser_t p;
    edge_parser_init(&p, onFrame, &cap);
    edge_parser_feed_buf(&p, buf, n);
    return cap;
}

}  // namespace

// ---- §1.5:上下行 TYPE 段不相交。一旦相交,错帧即可伪装成合法的对向帧被执行 ----
TEST(Contract, UplinkAndDownlinkSegmentsAreDisjoint) {
    for (int t = 0; t <= 0xFF; ++t) {
        const uint8_t type = static_cast<uint8_t>(t);
        EXPECT_FALSE(EDGE_IS_UPLINK(type) && EDGE_IS_DOWNLINK(type))
            << "TYPE 0x" << std::hex << t << " 同时属于上行与下行段";
    }
}

TEST(Contract, EveryDefinedTypeSitsInItsOwnSegment) {
    const uint8_t uplink[]   = {EDGE_TYPE_DHT11,  EDGE_TYPE_BH1750,     EDGE_TYPE_HEARTBEAT,
                                EDGE_TYPE_STATUS, EDGE_TYPE_QUERY_RESP, EDGE_TYPE_ACK};
    const uint8_t downlink[] = {EDGE_TYPE_QUERY_LIGHT, EDGE_TYPE_QUERY_TH, EDGE_TYPE_SET_PERIOD};

    for (uint8_t t : uplink) {
        EXPECT_TRUE(EDGE_IS_UPLINK(t)) << "上行 TYPE 0x" << std::hex << int(t) << " 不在上行段";
        EXPECT_FALSE(EDGE_IS_DOWNLINK(t));
    }
    for (uint8_t t : downlink) {
        EXPECT_TRUE(EDGE_IS_DOWNLINK(t)) << "下行 TYPE 0x" << std::hex << int(t) << " 不在下行段";
        EXPECT_FALSE(EDGE_IS_UPLINK(t));
    }
}

// 0x00 / 0xFF 保留为非法:全 0 与全 1 最易因空线、短路被误读
TEST(Contract, ReservedTypesAreNotInAnySegment) {
    EXPECT_FALSE(EDGE_IS_UPLINK(EDGE_TYPE_INVALID_LO));
    EXPECT_FALSE(EDGE_IS_DOWNLINK(EDGE_TYPE_INVALID_LO));
    EXPECT_FALSE(EDGE_IS_UPLINK(EDGE_TYPE_INVALID_HI));
    EXPECT_FALSE(EDGE_IS_DOWNLINK(EDGE_TYPE_INVALID_HI));
    EXPECT_LT(edge_min_payload_len(EDGE_TYPE_INVALID_LO), 0);
    EXPECT_LT(edge_min_payload_len(EDGE_TYPE_INVALID_HI), 0);
}

// ---- payload 长度校验 ----
TEST(Contract, MinPayloadLenMatchesTypeDictionary) {
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_DHT11), 5);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_BH1750), 2);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_HEARTBEAT), 0);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_STATUS), 1);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_QUERY_RESP), 2);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_ACK), 2);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_QUERY_LIGHT), 1);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_QUERY_TH), 1);
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_SET_PERIOD), 3);
    EXPECT_LT(edge_min_payload_len(0x99), 0) << "未定义 TYPE 必须返回负数";
}

TEST(Contract, PayloadLenOkRejectsShortAndUnknown) {
    EXPECT_TRUE(edge_payload_len_ok(EDGE_TYPE_ACK, 2));
    EXPECT_TRUE(edge_payload_len_ok(EDGE_TYPE_QUERY_RESP, 6)) << "0x05 变长 只约束下限";
    EXPECT_FALSE(edge_payload_len_ok(EDGE_TYPE_ACK, 1)) << "少一个字节就该拒";
    EXPECT_FALSE(edge_payload_len_ok(EDGE_TYPE_SET_PERIOD, 2)) << "缺周期低字节";
    EXPECT_FALSE(edge_payload_len_ok(0x99, 8)) << "未知 TYPE 一律拒";
}

// ---- 大端读写与量纲。移位写错不会崩溃,只会产生静默错值,故须逐位验证 ----
TEST(Contract, BigEndianReadWriteRoundTrip) {
    for (uint32_t v = 0; v <= 0xFFFF; v += 0x101) {  // 覆盖高低字节的各种组合
        uint8_t buf[2];
        edge_u16_be_write(buf, static_cast<uint16_t>(v));
        EXPECT_EQ(edge_u16_be_read(buf), v);
        EXPECT_EQ(buf[0], (v >> 8) & 0xFF) << "高字节必须在前(网络字节序)";
    }
}

TEST(Contract, BigEndianMatchesProtocolExamples) {
    const uint8_t period_2000s[] = {0x07, 0xD0};  // §7.5
    EXPECT_EQ(edge_u16_be_read(period_2000s), 2000);

    const uint8_t lux_400[] = {0x01, 0x90};  // §7.2
    EXPECT_EQ(edge_u16_be_read(lux_400), 400);

    const uint8_t temp_25_3[] = {0x00, 0xFD};  // §3.4:25.3℃ ×10 = 253
    EXPECT_EQ(edge_u16_be_read(temp_25_3), 253);
    EXPECT_DOUBLE_EQ(edge_u16_be_read(temp_25_3) / double(EDGE_TEMP_SCALE), 25.3);
}

// 周期单位为秒。以可执行断言固定该单位,防止文档与实现再次分歧。
TEST(Contract, PeriodIsSecondsAndRejectsZero) {
    EXPECT_FALSE(edge_period_s_valid(0)) << "周期 0 应判 BAD_PARAM(§6.3)";
    EXPECT_TRUE(edge_period_s_valid(EDGE_PERIOD_MIN_S));
    EXPECT_TRUE(edge_period_s_valid(EDGE_PERIOD_MAX_S));
    EXPECT_EQ(EDGE_PERIOD_MIN_S, 1u);
    EXPECT_EQ(EDGE_PERIOD_MAX_S, 65535u) << "uint16 秒 → 上限约 18.2 小时";
}

TEST(Contract, StatusBitmaskIsBitwiseNotEnum) {
    // §3.4:0x04 的 payload 是按位标志,不可当作单一数值解释
    const uint8_t both_ok = EDGE_STATUS_BIT_DHT11 | EDGE_STATUS_BIT_BH1750;
    EXPECT_EQ(both_ok, 0x03);
    EXPECT_TRUE(both_ok & EDGE_STATUS_BIT_DHT11);
    EXPECT_TRUE(both_ok & EDGE_STATUS_BIT_BH1750);

    const uint8_t light_failed = EDGE_STATUS_BIT_DHT11;
    EXPECT_TRUE(light_failed & EDGE_STATUS_BIT_DHT11);
    EXPECT_FALSE(light_failed & EDGE_STATUS_BIT_BH1750);
}

// 时序契约(§6.5):这些数值是两端共同遵守的,改动必须双方同意
TEST(Contract, TimingContractValues) {
    EXPECT_EQ(EDGE_ACK_TIMEOUT_MS, 500u);
    EXPECT_EQ(EDGE_MAX_RETRY, 3u);
}

// ------------------------------------------------------------
// 组帧参数校验
// ------------------------------------------------------------
TEST(Encode, RejectsInvalidArguments) {
    uint8_t out[EDGE_FRAME_MAX];
    std::vector<uint8_t> too_long(EDGE_PAYLOAD_MAX + 1, 0xAB);

    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_ACK, nullptr, 2, out), 0) << "payload=NULL 但长度非 0";
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_QUERY_RESP, too_long.data(),
                                static_cast<uint8_t>(too_long.size()), out),
              0)
        << "payload 超 EDGE_PAYLOAD_MAX";

    const uint8_t p[] = {0x01};
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_ACK, p, 1, nullptr), 0) << "out=NULL";
}

TEST(Encode, FrameLengthMatchesSpec) {
    uint8_t out[EDGE_FRAME_MAX];

    // §1.4:最小帧 6 字节(心跳)最大帧 69 字节
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, out), EDGE_FRAME_MIN);

    std::vector<uint8_t> max_payload(EDGE_PAYLOAD_MAX, 0x5A);
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_QUERY_RESP, max_payload.data(),
                                static_cast<uint8_t>(max_payload.size()), out),
              EDGE_FRAME_MAX);

    // LEN 字段 = 1 + payload 长度
    EXPECT_EQ(out[2], EDGE_LEN_MAX);
    EXPECT_EQ(out[0], EDGE_HDR0);
    EXPECT_EQ(out[1], EDGE_HDR1);
}

// ------------------------------------------------------------
// FSM 性质
// ------------------------------------------------------------
TEST(Parser, EveryTypeSurvivesRoundTrip) {
    const uint8_t types[] = {EDGE_TYPE_DHT11,      EDGE_TYPE_BH1750,      EDGE_TYPE_HEARTBEAT,
                             EDGE_TYPE_STATUS,     EDGE_TYPE_QUERY_RESP,  EDGE_TYPE_ACK,
                             EDGE_TYPE_QUERY_LIGHT, EDGE_TYPE_QUERY_TH,   EDGE_TYPE_SET_PERIOD};
    for (uint8_t t : types) {
        const int need = edge_min_payload_len(t);
        ASSERT_GE(need, 0);
        std::vector<uint8_t> payload(static_cast<std::size_t>(need), 0x42);

        const Capture cap = feedFrame(t, payload);
        EXPECT_EQ(cap.frames, 1) << "TYPE 0x" << std::hex << int(t);
        EXPECT_EQ(cap.type, t);
        EXPECT_EQ(cap.payload, payload);
    }
}

// §5.2 resync:任何错误路径都汇聚到同一个安全起点,最坏只丢 1 帧。
// 这条性质是 FSM 不卡死的全部依据,值得用暴力方式验一遍。
TEST(Parser, ResyncsFromArbitraryGarbageBeforeEveryFrame) {
    uint8_t good[EDGE_FRAME_MAX];
    const uint8_t payload[] = {0x09, 0x00};
    const uint8_t n = edge_frame_encode(EDGE_TYPE_ACK, payload, 2, good);

    // 用每一种可能的单字节噪声做前缀,好帧都必须照常解出
    for (int g = 0; g <= 0xFF; ++g) {
        Capture       cap;
        edge_parser_t p;
        edge_parser_init(&p, onFrame, &cap);

        const uint8_t garbage = static_cast<uint8_t>(g);
        edge_parser_feed(&p, garbage);
        edge_parser_feed_buf(&p, good, n);

        EXPECT_EQ(cap.frames, 1) << "噪声前缀 0x" << std::hex << g << " 之后好帧没解出";
        EXPECT_EQ(cap.type, EDGE_TYPE_ACK);
    }
}

// 单比特翻转必须被 CRC 抓住 —— 这是「电磁干扰下不误用坏数据」的底线
TEST(Parser, SingleBitFlipAnywhereIsCaught) {
    uint8_t original[EDGE_FRAME_MAX];
    const uint8_t payload[] = {0x07, 0x00, 0x01, 0x90};
    const uint8_t n = edge_frame_encode(EDGE_TYPE_QUERY_RESP, payload, 4, original);

    for (uint8_t pos = 0; pos < n; ++pos) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> corrupt(original, original + n);
            corrupt[pos] = static_cast<uint8_t>(corrupt[pos] ^ (1u << bit));

            Capture       cap;
            edge_parser_t p;
            edge_parser_init(&p, onFrame, &cap);
            edge_parser_feed_buf(&p, corrupt.data(), corrupt.size());

            // 帧头位翻转 → 帧根本不被识别;其余位翻转 → LEN 越界或 CRC 失配
            // 无论走哪条路 都不允许「交付出一个内容被改过的帧」
            if (cap.frames > 0) {
                ADD_FAILURE() << "第 " << int(pos) << " 字节第 " << bit
                              << " 位翻转后仍交付了帧 —— 坏数据会被当真";
            }
        }
    }
}

TEST(Parser, StatsCountEachErrorKindSeparately) {
    Capture       cap;
    edge_parser_t p;
    edge_parser_init(&p, onFrame, &cap);

    const uint8_t len_zero[]    = {EDGE_HDR0, EDGE_HDR1, 0x00};
    const uint8_t len_too_big[] = {EDGE_HDR0, EDGE_HDR1, 0x41};
    edge_parser_feed_buf(&p, len_zero, sizeof len_zero);
    edge_parser_feed_buf(&p, len_too_big, sizeof len_too_big);

    EXPECT_EQ(p.stats.len_err, 2u);
    EXPECT_EQ(p.stats.crc_err, 0u);
    EXPECT_EQ(p.stats.frames_ok, 0u);
    EXPECT_EQ(p.stats.resync, 2u) << "每次错误都应回到安全起点";
}

// 协议层不做 I/O、也不做业务判断:未知 TYPE 只要结构合法就照常交付,
// 由上层决定怎么处理。关注点分离 —— 协议层稳定 业务层迭代不牵动它。
TEST(Parser, DeliversStructurallyValidFrameWithUnknownType) {
    const uint8_t unknown_type = 0x99;
    const Capture cap = feedFrame(unknown_type, {0x11, 0x22});

    EXPECT_EQ(cap.frames, 1) << "协议层不该替业务层否决未知 TYPE";
    EXPECT_EQ(cap.type, unknown_type);
}

TEST(Parser, NullCallbackOnlyCountsAndDoesNotCrash) {
    uint8_t frame[EDGE_FRAME_MAX];
    const uint8_t n = edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, frame);

    edge_parser_t p;
    edge_parser_init(&p, nullptr, nullptr);  // 只统计不交付(链路探测场景)
    edge_parser_feed_buf(&p, frame, n);

    EXPECT_EQ(p.stats.frames_ok, 1u);
}

// 一字节一字节喂 与 一次喂一整批,结果必须完全一致。
// 网关是批量 read 后逐字节喂,节点是流缓冲攒一段再喂,两种节奏都得对。
TEST(Parser, ByteAtATimeEqualsBulkFeed) {
    uint8_t f1[EDGE_FRAME_MAX], f2[EDGE_FRAME_MAX];
    const uint8_t pl[] = {0x09, 0x00};
    const uint8_t n1 = edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, f1);
    const uint8_t n2 = edge_frame_encode(EDGE_TYPE_ACK, pl, 2, f2);

    std::vector<uint8_t> stream(f1, f1 + n1);
    stream.insert(stream.end(), f2, f2 + n2);

    Capture       bulk_cap, byte_cap;
    edge_parser_t bulk, byte;
    edge_parser_init(&bulk, onFrame, &bulk_cap);
    edge_parser_init(&byte, onFrame, &byte_cap);

    edge_parser_feed_buf(&bulk, stream.data(), stream.size());
    for (uint8_t b : stream) edge_parser_feed(&byte, b);

    EXPECT_EQ(bulk_cap.frames, 2);
    EXPECT_EQ(byte_cap.frames, bulk_cap.frames);
    EXPECT_EQ(byte_cap.type, bulk_cap.type);
    EXPECT_EQ(byte_cap.payload, bulk_cap.payload);
}
