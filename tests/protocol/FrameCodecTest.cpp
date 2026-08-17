// 网关 FrameCodec C++ 封装测试。buildFrame 接收 size_t 长度的 vector，底层共享 C
// 编码器只接受 uint8_t，因此本文件重点验证窄化前边界检查和逐字节透明性。

#include "gateway/protocol/FrameCodec.h"

#include <gtest/gtest.h>

#include <vector>

using gateway::buildFrame;

namespace {

// 生成长度为 n、内容固定为 0xAB 的负载，避免各边界用例重复构造数据。
std::vector<uint8_t> filled(std::size_t n) {
    return std::vector<uint8_t>(n, 0xAB);
}

} // namespace

// 帧固定开销为帧头、长度、类型和 CRC，共六字节。
TEST(BuildFrame, ValidPayloadLengthsProduceExpectedFrameSize) {
    EXPECT_EQ(buildFrame(EDGE_TYPE_HEARTBEAT, {}).size(), EDGE_FRAME_MIN);
    EXPECT_EQ(buildFrame(EDGE_TYPE_ACK, filled(2)).size(), 8u);

    const auto max_frame =
        buildFrame(EDGE_TYPE_QUERY_RESP, filled(EDGE_PAYLOAD_MAX)); // 最大合法帧。
    ASSERT_EQ(max_frame.size(), EDGE_FRAME_MAX);
    EXPECT_EQ(max_frame[2], EDGE_LEN_MAX) << "LEN = 1(TYPE) + 63";
}

// 在窄化前拒绝超长 vector，尤其覆盖转换后会回绕到合法长度的输入。
TEST(BuildFrame, OversizedPayloadIsRejectedNotTruncated) {
    struct Case {
        std::size_t size; // 输入 vector 的真实 size_t 长度。
        const char* why;  // 若先窄化为 uint8_t 会产生的具体风险。
    };
    const Case cases[] = {
        {EDGE_PAYLOAD_MAX + 1u, "刚过上限"},
        {255u, "uint8_t 上限,窄化后仍越界"},
        {256u, "窄化成 0 —— 会退化成空 payload 帧"},
        {257u, "窄化成 1 —— 会组出 1 字节 payload 的帧"},
        {300u, "窄化成 44 —— 会组出前 44 字节的帧"},
        {512u, "窄化成 0"},
    };

    for (const Case& c : cases) {
        EXPECT_TRUE(buildFrame(EDGE_TYPE_QUERY_RESP, filled(c.size)).empty())
            << "payload.size()=" << c.size << " (" << c.why << ")";
    }
}

// C++ 封装不得改变共享 C 编码器生成的任何字节。
TEST(BuildFrame, MatchesSharedEncoderByteForByte) {
    const std::vector<uint8_t> payload = {0x09, 0x07, 0xD0}; // seq 与两字节周期组成的代表负载。

    uint8_t expected[EDGE_FRAME_MAX]; // 共享 C 编码器写入的基准帧。
    const uint8_t n = edge_frame_encode(EDGE_TYPE_SET_PERIOD, payload.data(),
                                        static_cast<uint8_t>(payload.size()), expected);

    EXPECT_EQ(buildFrame(EDGE_TYPE_SET_PERIOD, payload),
              std::vector<uint8_t>(expected, expected + n));
}

// 最大合法帧也必须被流式解析器完整还原。
TEST(BuildFrame, RoundTripsThroughParser) {
    const std::vector<uint8_t> payload = filled(EDGE_PAYLOAD_MAX); // 最大负载原文。
    const auto frame = buildFrame(EDGE_TYPE_QUERY_RESP, payload);  // 待回环编码帧。
    ASSERT_FALSE(frame.empty());

    gateway::FrameParser parser; // 网关侧增量解析器。
    int delivered = 0;           // 回调实际触发次数。
    gateway::Frame got{};        // 回调最后交付的类型和负载。
    parser.setOnFrame([&](const gateway::Frame& f) {
        delivered++;
        got = f;
    });
    parser.feed(frame.data(), frame.size());

    ASSERT_EQ(delivered, 1);
    EXPECT_EQ(got.type, EDGE_TYPE_QUERY_RESP);
    EXPECT_EQ(got.payload, payload);
    EXPECT_EQ(parser.stats().frames_ok, 1u);
    EXPECT_EQ(parser.stats().crc_err, 0u);
}
