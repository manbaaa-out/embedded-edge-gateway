// edge_proto 的金标准一致性测试。网关与固件仓库共享同一组 CSV 输入，以独立测试程序
// 验证 CRC、编码帧和解析统计；向量是跨语言实现之间的可执行协议基准。

#include "VectorLoader.h"

#include "edge_proto/edge_crc16.h"
#include "edge_proto/edge_frame.h"

#include <gtest/gtest.h>

using namespace gwtest;

namespace {

// Capture 保存一个解析器实例的交付结果。frames 记录回调次数；last_type 和
// last_payload 保存最后一帧，用于同时覆盖单帧和含噪流向量。
struct Capture {
    int frames = 0;                    // 已交付完整帧数。
    uint8_t last_type = 0;             // 最后一帧 TYPE。
    std::vector<uint8_t> last_payload; // 最后一帧负载副本。
};

// edge_parser_t 的 C 回调：type/payload/len 是解析结果，user 指向当前用例的 Capture。
void onFrame(uint8_t type, const uint8_t* payload, uint8_t len, void* user) {
    auto* c = static_cast<Capture*>(user); // 恢复本解析器的结果容器。
    c->frames++;
    c->last_type = type;
    c->last_payload = std::vector<uint8_t>(payload, payload + len);
}

} // namespace

// crc16.csv 每行给出输入、期望 CRC 和说明；至少保留协议定义的四个基准向量。
TEST(Conformance, Crc16Vectors) {
    const auto rows = loadVectors("protocol/vectors/crc16.csv", 3); // 全部 CRC 金标准行。
    ASSERT_GE(rows.size(), 4u) << "§4.3 的四条向量一条都不能少";

    for (const auto& r : rows) {
        const auto input = parseHex(r[0]);        // 待计算的原始字节序列。
        const uint16_t expected = parseU16(r[1]); // CSV 声明的 CRC16-MODBUS 结果。
        const auto& note = r[2];                  // 失败时显示的向量说明。

        const uint16_t got =
            edge_crc16(input.empty() ? nullptr : input.data(), input.size()); // 实际 CRC。
        EXPECT_EQ(got, expected) << "输入 [" << toHex(input) << "] (" << note << ")";
    }
}

// 接收端的逐字节更新必须与发送端的一次性计算等价。
TEST(Conformance, Crc16IncrementalEqualsOneShot) {
    const auto rows = loadVectors("protocol/vectors/crc16.csv", 3); // 同一批输入同时走两种 API。
    for (const auto& r : rows) {
        const auto input = parseHex(r[0]); // 当前向量字节。

        uint16_t incremental = EDGE_CRC16_INIT; // 接收端逐字节累积状态。
        for (uint8_t b : input)
            incremental = edge_crc16_update(incremental, b);

        EXPECT_EQ(incremental, edge_crc16(input.empty() ? nullptr : input.data(), input.size()))
            << "输入 [" << toHex(input) << "]";
    }
}

// 金标准包含正确帧和错误输入，同时校验交付内容与错误计数。
TEST(Conformance, FrameDecode) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8); // 合法、损坏和含噪帧向量。

    for (const auto& r : rows) {
        const std::string& name = r[0];    // 稳定的向量标识，x_ 前缀表示错误输入。
        const auto bytes = parseHex(r[1]); // 整段待喂入解析器的字节流。
        const int expect_frames = std::stoi(r[2]);  // 期望交付帧数。
        const auto expect_payload = parseHex(r[4]); // 成功时最后一帧负载。
        const uint32_t expect_len_err = static_cast<uint32_t>(std::stoul(r[5])); // 长度错误数。
        const uint32_t expect_crc_err = static_cast<uint32_t>(std::stoul(r[6])); // CRC 错误数。
        const std::string& note = r[7]; // 人类可读场景说明。

        SCOPED_TRACE(name + " —— " + note);

        Capture cap;     // 当前向量的交付结果。
        edge_parser_t p; // 每行重新初始化，统计不跨向量累计。
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

// x_ 行表示噪声或损坏输入；其余向量均可由类型和负载反向编码。
TEST(Conformance, FrameEncodeRoundTrip) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8); // 编码基准来源。
    int checked = 0; // 实际参与编码回环的合法向量数。

    for (const auto& r : rows) {
        const std::string& name = r[0]; // 当前向量标识。
        if (name.rfind("x_", 0) == 0) continue;

        const auto expect_frame = parseHex(r[1]); // CSV 中的完整金标准帧。
        const uint8_t type = static_cast<uint8_t>(parseU16(r[3])); // 待编码 TYPE。
        const auto payload = parseHex(r[4]);                       // 待编码负载。

        SCOPED_TRACE(name);

        uint8_t out[EDGE_FRAME_MAX]; // 共享编码器输出缓冲。
        const uint8_t n = edge_frame_encode(type, payload.empty() ? nullptr : payload.data(),
                                            static_cast<uint8_t>(payload.size()), out);
        ASSERT_GT(n, 0) << "组帧失败";

        EXPECT_EQ(toHex(std::vector<uint8_t>(out, out + n)), toHex(expect_frame));
        ++checked;
    }
    EXPECT_GE(checked, 10) << "参与编码回环的向量太少 覆盖不足";
}

// 对每个合法向量，解码结果重新编码后应恢复原帧。
TEST(Conformance, DecodeThenEncodeIsIdentity) {
    const auto rows = loadVectors("protocol/vectors/frames.csv", 8); // 只处理可交付的合法行。

    for (const auto& r : rows) {
        if (r[0].rfind("x_", 0) == 0) continue;
        const auto frame = parseHex(r[1]); // 先解码、后重新编码的原始帧。
        SCOPED_TRACE(r[0]);

        Capture cap;     // 解码得到的类型和负载。
        edge_parser_t p; // 当前帧独立解析器。
        edge_parser_init(&p, onFrame, &cap);
        edge_parser_feed_buf(&p, frame.data(), frame.size());
        ASSERT_EQ(cap.frames, 1);

        uint8_t out[EDGE_FRAME_MAX]; // 由解码结果生成的回编码帧。
        const uint8_t n = edge_frame_encode(
            cap.last_type, cap.last_payload.empty() ? nullptr : cap.last_payload.data(),
            static_cast<uint8_t>(cap.last_payload.size()), out);
        EXPECT_EQ(toHex(std::vector<uint8_t>(out, out + n)), toHex(frame));
    }
}
