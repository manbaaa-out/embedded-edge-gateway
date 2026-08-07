// 协议一致性测试:逐行跑 protocol/vectors/ 下的金标准向量。
//
// 这是「网关与 STM32 节点协议一致」的机器化保证:两个仓库跑同一份数据文件,
// 任一端改坏编解码,双方 CI 均会失败。节点侧的对等实现在 STM32Project/Protocol/test/。

#include "VectorLoader.h"

#include "edge_proto/edge_crc16.h"
#include "edge_proto/edge_frame.h"

#include <gtest/gtest.h>

using namespace gwtest;

namespace {

// 收帧回调经 void* user 把结果收集到此,协议层不引入全局变量
struct Capture {
    int                  frames = 0;
    uint8_t              last_type = 0;
    std::vector<uint8_t> last_payload;
};

void onFrame(uint8_t type, const uint8_t* payload, uint8_t len, void* user) {
    auto* c = static_cast<Capture*>(user);
    c->frames++;
    c->last_type    = type;
    c->last_payload = std::vector<uint8_t>(payload, payload + len);
}

}  // namespace

// ---- CRC16-MODBUS 向量(docs/protocol.md §4.3)----
TEST(Conformance, Crc16Vectors) {
    const auto rows = loadVectors("protocol/vectors/crc16.csv", 3);
    ASSERT_GE(rows.size(), 4u) << "§4.3 的四条向量一条都不能少";

    for (const auto& r : rows) {
        const auto     input    = parseHex(r[0]);
        const uint16_t expected = parseU16(r[1]);
        const auto&    note     = r[2];

        const uint16_t got = edge_crc16(input.empty() ? nullptr : input.data(), input.size());
        EXPECT_EQ(got, expected) << "输入 [" << toHex(input) << "] (" << note << ")";
    }
}

// 逐字节累加与一次性计算必须等价:接收端 FSM 走前者,发送端组帧走后者。
// 两者不等价会导致自己发出的帧无法通过自身校验。
TEST(Conformance, Crc16IncrementalEqualsOneShot) {
    const auto rows = loadVectors("protocol/vectors/crc16.csv", 3);
    for (const auto& r : rows) {
        const auto input = parseHex(r[0]);

        uint16_t incremental = EDGE_CRC16_INIT;
        for (uint8_t b : input) incremental = edge_crc16_update(incremental, b);

        EXPECT_EQ(incremental, edge_crc16(input.empty() ? nullptr : input.data(), input.size()))
            << "输入 [" << toHex(input) << "]";
    }
}

// ---- 解码:把金标准帧逐字节喂进 FSM,比对交付结果与错误统计 ----
TEST(Conformance, FrameDecode) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8);

    for (const auto& r : rows) {
        const std::string& name            = r[0];
        const auto         bytes           = parseHex(r[1]);
        const int          expect_frames   = std::stoi(r[2]);
        const auto         expect_payload  = parseHex(r[4]);
        const uint32_t     expect_len_err  = static_cast<uint32_t>(std::stoul(r[5]));
        const uint32_t     expect_crc_err  = static_cast<uint32_t>(std::stoul(r[6]));
        const std::string& note            = r[7];

        SCOPED_TRACE(name + " —— " + note);

        Capture       cap;
        edge_parser_t p;
        edge_parser_init(&p, onFrame, &cap);
        edge_parser_feed_buf(&p, bytes.data(), bytes.size());

        EXPECT_EQ(cap.frames, expect_frames) << "交付帧数不符";
        EXPECT_EQ(p.stats.len_err, expect_len_err) << "LEN 越界计数不符";
        EXPECT_EQ(p.stats.crc_err, expect_crc_err) << "CRC 失配计数不符";
        EXPECT_EQ(p.stats.frames_ok, static_cast<uint32_t>(expect_frames));

        if (expect_frames > 0) {
            EXPECT_EQ(cap.last_type, parseU16(r[3])) << "TYPE 不符";
            EXPECT_EQ(toHex(cap.last_payload), toHex(expect_payload)) << "payload 不符";
        }
    }
}

// ---- 编码:对干净的单帧行反向组帧,要求与金标准逐字节相同。
// x_ 前缀的行含噪声或错误,不参与编码回环(见 frames.csv 表头) ----
TEST(Conformance, FrameEncodeRoundTrip) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8);
    int        checked = 0;

    for (const auto& r : rows) {
        const std::string& name = r[0];
        if (name.rfind("x_", 0) == 0) continue;

        const auto    expect_frame = parseHex(r[1]);
        const uint8_t type         = static_cast<uint8_t>(parseU16(r[3]));
        const auto    payload      = parseHex(r[4]);

        SCOPED_TRACE(name);

        uint8_t out[EDGE_FRAME_MAX];
        const uint8_t n = edge_frame_encode(type, payload.empty() ? nullptr : payload.data(),
                                            static_cast<uint8_t>(payload.size()), out);
        ASSERT_GT(n, 0) << "组帧失败";

        EXPECT_EQ(toHex(std::vector<uint8_t>(out, out + n)), toHex(expect_frame));
        ++checked;
    }
    EXPECT_GE(checked, 10) << "参与编码回环的向量太少 覆盖不足";
}

// 解码交付的 payload 再编回去,必须还原成原帧 —— 编解码互为逆运算。
TEST(Conformance, DecodeThenEncodeIsIdentity) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8);

    for (const auto& r : rows) {
        if (r[0].rfind("x_", 0) == 0) continue;
        const auto frame = parseHex(r[1]);
        SCOPED_TRACE(r[0]);

        Capture       cap;
        edge_parser_t p;
        edge_parser_init(&p, onFrame, &cap);
        edge_parser_feed_buf(&p, frame.data(), frame.size());
        ASSERT_EQ(cap.frames, 1);

        uint8_t out[EDGE_FRAME_MAX];
        const uint8_t n =
            edge_frame_encode(cap.last_type,
                              cap.last_payload.empty() ? nullptr : cap.last_payload.data(),
                              static_cast<uint8_t>(cap.last_payload.size()), out);
        EXPECT_EQ(toHex(std::vector<uint8_t>(out, out + n)), toHex(frame));
    }
}
