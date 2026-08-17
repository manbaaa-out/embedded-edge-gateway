// edge_proto 的性质测试。与 ConformanceTest 的固定金标准不同，本文件枚举类型空间、
// 字节值和错误变换，验证分段不相交、长度字典、字节序、编码防护与 FSM 恢复能力。

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

// Capture 是单个解析器的回调结果：frames 统计交付次数，type/payload 保存最后一帧。
struct Capture {
    int frames = 0;               // 已交付完整帧数。
    uint8_t type = 0;             // 最后一次交付的 TYPE。
    std::vector<uint8_t> payload; // 最后一次交付的负载副本。
};

// C 解析器回调：type/p/len 描述一帧，user 指向当前 Capture，回调将负载立即复制。
void onFrame(uint8_t type, const uint8_t* p, uint8_t len, void* user) {
    auto* c = static_cast<Capture*>(user); // 恢复当前解析器对应的结果容器。
    c->frames++;
    c->type = type;
    c->payload.assign(p, p + len);
}

// 将给定 type/payload 编码后立即送入全新解析器，并返回该次回调结果。
Capture feedFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    uint8_t buf[EDGE_FRAME_MAX]; // 编码器输出的最大帧缓冲。
    uint8_t n = edge_frame_encode(type, payload.empty() ? nullptr : payload.data(), // 实际帧长。
                                  static_cast<uint8_t>(payload.size()), buf);
    Capture cap;     // 本次回环的交付结果。
    edge_parser_t p; // 不携带其他输入历史的新解析器。
    edge_parser_init(&p, onFrame, &cap);
    edge_parser_feed_buf(&p, buf, n);
    return cap;
}

} // namespace

// 上下行类型段必须互斥，防止同一类型在两端产生相反解释。
TEST(Contract, UplinkAndDownlinkSegmentsAreDisjoint) {
    for (int t = 0; t <= 0xFF; ++t) {
        const uint8_t type = static_cast<uint8_t>(t); // 当前完整一字节 TYPE 值。
        EXPECT_FALSE(EDGE_IS_UPLINK(type) && EDGE_IS_DOWNLINK(type))
            << "TYPE 0x" << std::hex << t << " 同时属于上行与下行段";
    }
}

// 每个已定义 TYPE 必须落入预期方向，且不能同时命中另一方向宏。
TEST(Contract, EveryDefinedTypeSitsInItsOwnSegment) {
    const uint8_t uplink[] = {
        EDGE_TYPE_DHT11,  EDGE_TYPE_BH1750,     EDGE_TYPE_HEARTBEAT, // 节点发往网关。
        EDGE_TYPE_STATUS, EDGE_TYPE_QUERY_RESP, EDGE_TYPE_ACK};
    const uint8_t downlink[] = {EDGE_TYPE_QUERY_LIGHT, EDGE_TYPE_QUERY_TH,
                                EDGE_TYPE_SET_PERIOD}; // 网关发往节点。

    for (uint8_t t : uplink) {
        EXPECT_TRUE(EDGE_IS_UPLINK(t)) << "上行 TYPE 0x" << std::hex << int(t) << " 不在上行段";
        EXPECT_FALSE(EDGE_IS_DOWNLINK(t));
    }
    for (uint8_t t : downlink) {
        EXPECT_TRUE(EDGE_IS_DOWNLINK(t)) << "下行 TYPE 0x" << std::hex << int(t) << " 不在下行段";
        EXPECT_FALSE(EDGE_IS_UPLINK(t));
    }
}

// 两个保留端点不属于任何方向，也没有负载契约。
TEST(Contract, ReservedTypesAreNotInAnySegment) {
    EXPECT_FALSE(EDGE_IS_UPLINK(EDGE_TYPE_INVALID_LO));
    EXPECT_FALSE(EDGE_IS_DOWNLINK(EDGE_TYPE_INVALID_LO));
    EXPECT_FALSE(EDGE_IS_UPLINK(EDGE_TYPE_INVALID_HI));
    EXPECT_FALSE(EDGE_IS_DOWNLINK(EDGE_TYPE_INVALID_HI));
    EXPECT_LT(edge_min_payload_len(EDGE_TYPE_INVALID_LO), 0);
    EXPECT_LT(edge_min_payload_len(EDGE_TYPE_INVALID_HI), 0);
}

// 类型字典只限定最小长度，允许已知前缀后追加兼容字段。
TEST(Contract, MinPayloadLenMatchesTypeDictionary) {
    EXPECT_EQ(edge_min_payload_len(EDGE_TYPE_DHT11), 4);
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

// edge_payload_len_ok 应接受达到最小值和兼容扩展，拒绝短帧及未知 TYPE。
TEST(Contract, PayloadLenOkRejectsShortAndUnknown) {
    EXPECT_TRUE(edge_payload_len_ok(EDGE_TYPE_ACK, 2));
    EXPECT_TRUE(edge_payload_len_ok(EDGE_TYPE_QUERY_RESP, 6)) << "0x05 变长 只约束下限";
    EXPECT_FALSE(edge_payload_len_ok(EDGE_TYPE_ACK, 1)) << "少一个字节就该拒";
    EXPECT_FALSE(edge_payload_len_ok(EDGE_TYPE_SET_PERIOD, 2)) << "缺周期低字节";
    EXPECT_FALSE(edge_payload_len_ok(0x99, 8)) << "未知 TYPE 一律拒";
}

// 两字节量统一采用大端序，覆盖全范围的读写互逆关系。
TEST(Contract, BigEndianReadWriteRoundTrip) {
    for (uint32_t v = 0; v <= 0xFFFF; v += 0x101) { // 同步推进高低字节，覆盖边界组合。
        uint8_t buf[2];                             // 当前 uint16_t 的大端编码字节。
        edge_u16_be_write(buf, static_cast<uint16_t>(v));
        EXPECT_EQ(edge_u16_be_read(buf), v);
        EXPECT_EQ(buf[0], (v >> 8) & 0xFF) << "高字节必须在前(网络字节序)";
    }
}

// 使用协议中的人类可读示例确认大端 helper 与秒、lux、十倍温度量纲一致。
TEST(Contract, BigEndianMatchesProtocolExamples) {
    const uint8_t period_2000s[] = {0x07, 0xD0}; // 2000 秒
    EXPECT_EQ(edge_u16_be_read(period_2000s), 2000);

    const uint8_t lux_400[] = {0x01, 0x90}; // 400 lux
    EXPECT_EQ(edge_u16_be_read(lux_400), 400);

    const uint8_t temp_25_3[] = {0x00, 0xFD}; // 温度采用十倍定标。
    EXPECT_EQ(edge_u16_be_read(temp_25_3), 253);
    EXPECT_DOUBLE_EQ(edge_u16_be_read(temp_25_3) / double(EDGE_TEMP_SCALE), 25.3);
}

// 周期字段以秒计，零值无效，最大值受 uint16_t 限制。
TEST(Contract, PeriodIsSecondsAndRejectsZero) {
    EXPECT_FALSE(edge_period_s_valid(0)) << "周期 0 应判 BAD_PARAM(§6.3)";
    EXPECT_TRUE(edge_period_s_valid(EDGE_PERIOD_MIN_S));
    EXPECT_TRUE(edge_period_s_valid(EDGE_PERIOD_MAX_S));
    EXPECT_EQ(EDGE_PERIOD_MIN_S, 1u);
    EXPECT_EQ(EDGE_PERIOD_MAX_S, 65535u) << "uint16 秒 → 上限约 18.2 小时";
}

// 状态字段是两个可组合 bit，而不是值域互斥的枚举。
TEST(Contract, StatusBitmaskIsBitwiseNotEnum) {
    // 两个传感器状态位可以独立组合。
    const uint8_t both_ok = EDGE_STATUS_BIT_DHT11 | EDGE_STATUS_BIT_BH1750; // 两位同时置位。
    EXPECT_EQ(both_ok, 0x03);
    EXPECT_TRUE(both_ok & EDGE_STATUS_BIT_DHT11);
    EXPECT_TRUE(both_ok & EDGE_STATUS_BIT_BH1750);

    const uint8_t light_failed = EDGE_STATUS_BIT_DHT11; // 仅温湿度位有效。
    EXPECT_TRUE(light_failed & EDGE_STATUS_BIT_DHT11);
    EXPECT_FALSE(light_failed & EDGE_STATUS_BIT_BH1750);
}

// 超时与最大重试次数属于两端共同遵守的协议常量。
TEST(Contract, TimingContractValues) {
    EXPECT_EQ(EDGE_ACK_TIMEOUT_MS, 500u);
    EXPECT_EQ(EDGE_MAX_RETRY, 3u);
}

// 编码器在写入输出缓冲区前拒绝无效指针和超长负载。
TEST(Encode, RejectsInvalidArguments) {
    uint8_t out[EDGE_FRAME_MAX]; // 合法输出指针，用于隔离其他参数错误。
    std::vector<uint8_t> too_long(EDGE_PAYLOAD_MAX + 1, 0xAB); // 刚超协议上限的负载。

    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_ACK, nullptr, 2, out), 0) << "payload=NULL 但长度非 0";
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_QUERY_RESP, too_long.data(),
                                static_cast<uint8_t>(too_long.size()), out),
              0)
        << "payload 超 EDGE_PAYLOAD_MAX";

    const uint8_t p[] = {0x01}; // 合法非空负载，用于单独验证空输出指针。
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_ACK, p, 1, nullptr), 0) << "out=NULL";
}

// 空负载和最大负载应分别生成 EDGE_FRAME_MIN/MAX，并写入正确帧头和 LEN。
TEST(Encode, FrameLengthMatchesSpec) {
    uint8_t out[EDGE_FRAME_MAX]; // 两次编码复用的最大输出缓冲。

    // 空负载和最大负载分别覆盖帧长的两个端点。
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, out), EDGE_FRAME_MIN);

    std::vector<uint8_t> max_payload(EDGE_PAYLOAD_MAX, 0x5A); // 恰好达到上限的合法负载。
    EXPECT_EQ(edge_frame_encode(EDGE_TYPE_QUERY_RESP, max_payload.data(),
                                static_cast<uint8_t>(max_payload.size()), out),
              EDGE_FRAME_MAX);

    // LEN 同时包含 TYPE 字节和负载。
    EXPECT_EQ(out[2], EDGE_LEN_MAX);
    EXPECT_EQ(out[0], EDGE_HDR0);
    EXPECT_EQ(out[1], EDGE_HDR1);
}

// 所有已定义类型经编码和解析后都应保持类型与负载不变。
TEST(Parser, EveryTypeSurvivesRoundTrip) {
    const uint8_t types[] = {
        EDGE_TYPE_DHT11,       EDGE_TYPE_BH1750,     EDGE_TYPE_HEARTBEAT, // 完整已定义 TYPE 集。
        EDGE_TYPE_STATUS,      EDGE_TYPE_QUERY_RESP, EDGE_TYPE_ACK,
        EDGE_TYPE_QUERY_LIGHT, EDGE_TYPE_QUERY_TH,   EDGE_TYPE_SET_PERIOD};
    for (uint8_t t : types) {
        const int need = edge_min_payload_len(t); // 当前类型满足字典所需的最小负载长度。
        ASSERT_GE(need, 0);
        std::vector<uint8_t> payload(static_cast<std::size_t>(need),
                                     0x42); // 只测试结构的占位负载。

        const Capture cap = feedFrame(t, payload); // 当前类型的回环结果。
        EXPECT_EQ(cap.frames, 1) << "TYPE 0x" << std::hex << int(t);
        EXPECT_EQ(cap.type, t);
        EXPECT_EQ(cap.payload, payload);
    }
}

// 任意单字节噪声后跟合法帧时，解析器都必须重新同步并交付该帧。
TEST(Parser, ResyncsFromArbitraryGarbageBeforeEveryFrame) {
    uint8_t good[EDGE_FRAME_MAX];           // 每种噪声后追加的同一合法 ACK 帧。
    const uint8_t payload[] = {0x09, 0x00}; // seq=9、结果 OK。
    const uint8_t n = edge_frame_encode(EDGE_TYPE_ACK, payload, 2, good); // 合法帧长。

    // 枚举完整字节空间，包含与帧头相同的噪声值。
    for (int g = 0; g <= 0xFF; ++g) {
        Capture cap;     // 当前噪声值下的交付结果。
        edge_parser_t p; // 每个前缀独立初始化，避免状态串扰。
        edge_parser_init(&p, onFrame, &cap);

        const uint8_t garbage = static_cast<uint8_t>(g); // 当前枚举的单字节前缀。
        edge_parser_feed(&p, garbage);
        edge_parser_feed_buf(&p, good, n);

        EXPECT_EQ(cap.frames, 1) << "噪声前缀 0x" << std::hex << g << " 之后好帧没解出";
        EXPECT_EQ(cap.type, EDGE_TYPE_ACK);
    }
}

// 枚举整帧的所有单比特翻转，任何损坏输入都不得作为有效帧交付。
TEST(Parser, SingleBitFlipAnywhereIsCaught) {
    uint8_t original[EDGE_FRAME_MAX];                   // 未损坏的查询响应基准帧。
    const uint8_t payload[] = {0x07, 0x00, 0x01, 0x90}; // seq、OK 和 400 lux。
    const uint8_t n = edge_frame_encode(EDGE_TYPE_QUERY_RESP, payload, 4, original); // 待枚举帧长。

    for (uint8_t pos = 0; pos < n; ++pos) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<uint8_t> corrupt(original, original + n); // 本轮只翻转一个 bit 的副本。
            corrupt[pos] = static_cast<uint8_t>(corrupt[pos] ^ (1u << bit));

            Capture cap;     // 损坏输入不应产生任何交付。
            edge_parser_t p; // 每个翻转位置使用干净状态机。
            edge_parser_init(&p, onFrame, &cap);
            edge_parser_feed_buf(&p, corrupt.data(), corrupt.size());

            // 错误可能走未识别、长度异常或 CRC 失配路径，交付数必须保持为零。
            if (cap.frames > 0) {
                ADD_FAILURE() << "第 " << int(pos) << " 字节第 " << bit
                              << " 位翻转后仍交付了帧 —— 坏数据会被当真";
            }
        }
    }
}

// 两个非法 LEN 输入应只增加 len_err/resync，不污染 crc_err 或 frames_ok。
TEST(Parser, StatsCountEachErrorKindSeparately) {
    Capture cap;     // 验证错误输入没有触发业务回调。
    edge_parser_t p; // 在同一实例中累计两次长度错误。
    edge_parser_init(&p, onFrame, &cap);

    const uint8_t len_zero[] = {EDGE_HDR0, EDGE_HDR1, 0x00};    // 小于 LEN 最小值 1。
    const uint8_t len_too_big[] = {EDGE_HDR0, EDGE_HDR1, 0x41}; // 大于 LEN 最大值 64。
    edge_parser_feed_buf(&p, len_zero, sizeof len_zero);
    edge_parser_feed_buf(&p, len_too_big, sizeof len_too_big);

    EXPECT_EQ(p.stats.len_err, 2u);
    EXPECT_EQ(p.stats.crc_err, 0u);
    EXPECT_EQ(p.stats.frames_ok, 0u);
    EXPECT_EQ(p.stats.resync, 2u) << "每次错误都应回到安全起点";
}

// 解析器只验证帧结构；未知业务类型交由上层决定是否接受。
TEST(Parser, DeliversStructurallyValidFrameWithUnknownType) {
    const uint8_t unknown_type = 0x99; // 不在业务字典中的结构合法 TYPE。
    const Capture cap = feedFrame(unknown_type, {0x11, 0x22}); // 协议层回环结果。

    EXPECT_EQ(cap.frames, 1) << "协议层不该替业务层否决未知 TYPE";
    EXPECT_EQ(cap.type, unknown_type);
}

// 回调可为空：解析器仍应完成校验和统计，且不能解引用空 user 指针。
TEST(Parser, NullCallbackOnlyCountsAndDoesNotCrash) {
    uint8_t frame[EDGE_FRAME_MAX]; // 合法心跳帧输出。
    const uint8_t n = edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, frame); // 实际长度。

    edge_parser_t p;                        // 不安装回调的解析器实例。
    edge_parser_init(&p, nullptr, nullptr); // 无回调时仍维护解析统计。
    edge_parser_feed_buf(&p, frame, n);

    EXPECT_EQ(p.stats.frames_ok, 1u);
}

// 批量输入和逐字节输入必须产生相同的交付结果。
TEST(Parser, ByteAtATimeEqualsBulkFeed) {
    uint8_t f1[EDGE_FRAME_MAX], f2[EDGE_FRAME_MAX]; // 心跳和 ACK 编码缓冲。
    const uint8_t pl[] = {0x09, 0x00};              // ACK 的 seq 和 OK 结果码。
    const uint8_t n1 = edge_frame_encode(EDGE_TYPE_HEARTBEAT, nullptr, 0, f1); // 第一帧长度。
    const uint8_t n2 = edge_frame_encode(EDGE_TYPE_ACK, pl, 2, f2);            // 第二帧长度。

    std::vector<uint8_t> stream(f1, f1 + n1); // 两帧首尾相接的输入流。
    stream.insert(stream.end(), f2, f2 + n2);

    Capture bulk_cap, byte_cap; // 两种 feed API 的交付结果。
    edge_parser_t bulk, byte;   // 批量输入和逐字节输入的独立解析器。
    edge_parser_init(&bulk, onFrame, &bulk_cap);
    edge_parser_init(&byte, onFrame, &byte_cap);

    edge_parser_feed_buf(&bulk, stream.data(), stream.size());
    for (uint8_t b : stream)
        edge_parser_feed(&byte, b);

    EXPECT_EQ(bulk_cap.frames, 2);
    EXPECT_EQ(byte_cap.frames, bulk_cap.frames);
    EXPECT_EQ(byte_cap.type, bulk_cap.type);
    EXPECT_EQ(byte_cap.payload, bulk_cap.payload);
}
