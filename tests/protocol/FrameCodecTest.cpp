// 网关侧 C++ 封装的边界测试。
//
// 同目录的另两个 target 测的是与固件共享的 C 核心;本文件测的是网关独有的那层胶水。
// 胶水层最容易出的不是逻辑错,而是类型宽度错:C 接口按 uint8_t 收长度,std::vector
// 按 size_t 给长度,两者之间那次窄化一旦无人把关,过长的 payload 就会被截成合法值。

#include "gateway/protocol/FrameCodec.h"

#include <gtest/gtest.h>

#include <vector>

using gateway::buildFrame;

namespace {

std::vector<uint8_t> filled(std::size_t n) { return std::vector<uint8_t>(n, 0xAB); }

}  // namespace

// 合法区间内,整帧长度应为 payload + 6(帧头 2 + LEN 1 + TYPE 1 + CRC 2)
TEST(BuildFrame, ValidPayloadLengthsProduceExpectedFrameSize) {
    EXPECT_EQ(buildFrame(EDGE_TYPE_HEARTBEAT, {}).size(), EDGE_FRAME_MIN);
    EXPECT_EQ(buildFrame(EDGE_TYPE_ACK, filled(2)).size(), 8u);

    const auto max_frame = buildFrame(EDGE_TYPE_QUERY_RESP, filled(EDGE_PAYLOAD_MAX));
    ASSERT_EQ(max_frame.size(), EDGE_FRAME_MAX);
    EXPECT_EQ(max_frame[2], EDGE_LEN_MAX) << "LEN = 1(TYPE) + 63";
}

// 超出 EDGE_PAYLOAD_MAX 一律拒绝,且拒绝的判据必须是 size_t 而非窄化后的值。
//
// 256 的整数倍是这里唯一危险的输入:强转成 uint8_t 后落回合法区间,
// 会组出一个 CRC 自洽、收帧端无从察觉、内容却被腰斩的帧。
TEST(BuildFrame, OversizedPayloadIsRejectedNotTruncated) {
    struct Case {
        std::size_t size;
        const char* why;
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

// 合法路径下 C++ 封装与 C 接口必须产出逐字节相同的帧 —— 封装不得携带任何协议知识
TEST(BuildFrame, MatchesSharedEncoderByteForByte) {
    const std::vector<uint8_t> payload = {0x09, 0x07, 0xD0};

    uint8_t       expected[EDGE_FRAME_MAX];
    const uint8_t n = edge_frame_encode(EDGE_TYPE_SET_PERIOD, payload.data(),
                                        static_cast<uint8_t>(payload.size()), expected);

    EXPECT_EQ(buildFrame(EDGE_TYPE_SET_PERIOD, payload),
              std::vector<uint8_t>(expected, expected + n));
}

// 组出的帧必须能被收帧 FSM 原样解回来 —— 编解码互为逆运算
TEST(BuildFrame, RoundTripsThroughParser) {
    const std::vector<uint8_t> payload = filled(EDGE_PAYLOAD_MAX);
    const auto                 frame   = buildFrame(EDGE_TYPE_QUERY_RESP, payload);
    ASSERT_FALSE(frame.empty());

    gateway::FrameParser parser;
    int                  delivered = 0;
    gateway::Frame       got{};
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
