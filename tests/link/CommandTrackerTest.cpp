// CommandTracker 状态机测试：以可控时间点代替真实等待，验证序号分配、在途表销账、
// 固定间隔重试和最终失败动作；不经过串口，因此可精确断言每个 tick 的输出。

#include "gateway/link/CommandTracker.h"

#include <gtest/gtest.h>

using namespace gateway;
using namespace std::chrono_literals;

namespace {

// FakeClock 以创建时的真实 steady_clock 时间为基准，只由 advance(d) 增加逻辑偏移；
// now() 返回可直接传给 CommandTracker 的 TimePoint，d 表示本步推进的毫秒数。
class FakeClock {
public:
    CommandTracker::TimePoint now() const { return base_ + elapsed_; }
    void advance(std::chrono::milliseconds d) { elapsed_ += d; }

private:
    CommandTracker::TimePoint base_ = CommandTracker::Clock::now(); // 每个用例独立的时间原点。
    std::chrono::milliseconds elapsed_ = 0ms; // 从原点累计的人工偏移。
};

const std::vector<uint8_t> kPeriodArg = {0x07, 0xD0}; // set_period 的 2000 秒大端参数。

} // namespace

// 连续 track 应分配 0、1、2，并为每条命令保留独立在途记录。
TEST(CommandTracker, AssignsIncrementingSeq) {
    CommandTracker t; // 使用协议默认超时和重试次数的被测跟踪器。
    FakeClock clk;    // 三次登记发生在同一逻辑时刻。

    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now()), 0);
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 1);
    EXPECT_EQ(t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now()), 2);
    EXPECT_EQ(t.inflightCount(), 3u);
}

// 一字节序号在 255 后按无符号规则回绕。
TEST(CommandTracker, SeqWrapsAroundAt255) {
    CommandTracker t; // 被测一字节序号生成器。
    FakeClock clk;    // 本用例不推进时间，只验证序号空间。

    for (int i = 0; i < 255; ++i) {
        t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());
        t.onAck(static_cast<uint8_t>(i)); // 立即销账，避免已用序号留在在途表。
    }
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 255);
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 0) << "0xFF 之后应回绕到 0";
}

// 命中在途 seq 的 ACK 返回 true、删除唯一记录，并同步更新计数和 has 查询。
TEST(CommandTracker, AckMatchesAndClearsEntry) {
    CommandTracker t; // 保存待由 ACK 销账的唯一在途命令。
    FakeClock clk;    // 登记和应答发生在同一逻辑时刻。

    const uint8_t seq = t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now()); // 待匹配命令序号。
    ASSERT_TRUE(t.has(seq));

    EXPECT_TRUE(t.onAck(seq));
    EXPECT_FALSE(t.has(seq)) << "配对成功即销账";
    EXPECT_EQ(t.inflightCount(), 0u);
}

// 未登记或重复的应答不能影响仍在等待的命令。
TEST(CommandTracker, UnknownSeqAckIsRejected) {
    CommandTracker t; // 同时接受错误序号和正确序号的被测跟踪器。
    FakeClock clk;    // 本用例不涉及超时推进。
    const uint8_t seq = t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()); // 唯一合法的在途序号。

    EXPECT_FALSE(t.onAck(static_cast<uint8_t>(seq + 1))) << "不在表里的 seq 应被拒";
    EXPECT_TRUE(t.has(seq)) << "别人的应答不该影响这条";

    EXPECT_TRUE(t.onAck(seq));
    EXPECT_FALSE(t.onAck(seq)) << "重复的应答第二次应被拒";
}

// 节点返回何种结果码都代表传输闭环完成，跟踪器只负责销账。
TEST(CommandTracker, AckClearsRegardlessOfResultCode) {
    CommandTracker t; // 只按序号销账、不接收结果码的被测跟踪器。
    FakeClock clk;    // 命令尚未超时即收到应答。
    const uint8_t seq =
        t.track(EDGE_TYPE_SET_PERIOD, {0x00, 0x00}, clk.now()); // 节点可能拒绝的参数。

    EXPECT_TRUE(t.onAck(seq)); // rc 由上层解析，不属于 onAck 的输入。
    EXPECT_EQ(t.inflightCount(), 0u);
}

// 距登记时刻 499 ms 尚未达到 500 ms 阈值，不产生 resend 或 failed 动作。
TEST(CommandTracker, NoActionBeforeTimeout) {
    CommandTracker t(500ms, 3); // 显式超时和重试上限，便于边界计算。
    FakeClock clk;              // 精确推进到阈值前一毫秒。
    t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now());

    clk.advance(499ms);
    const auto a = t.tick(clk.now()); // 当前时刻应为空的动作批次。

    EXPECT_TRUE(a.resend.empty()) << "没到时限就不该重发";
    EXPECT_TRUE(a.failed.empty());
}

// 重试保留原序号和参数，使节点能够识别重复命令。
TEST(CommandTracker, ResendReusesSameSeqAndArgs) {
    CommandTracker t(500ms, 3); // 在首个超时窗口产生重发动作。
    FakeClock clk;              // 从登记时刻推进恰好 500 ms。
    const uint8_t seq =
        t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now()); // 初次发送分配的序号。

    clk.advance(500ms);
    const auto a = t.tick(clk.now()); // 首个超时产生的一条重发动作。

    ASSERT_EQ(a.resend.size(), 1u);
    EXPECT_EQ(a.resend[0].seq, seq) << "重发必须用同一个 seq(§6.2 幂等)";
    EXPECT_EQ(a.resend[0].type, EDGE_TYPE_SET_PERIOD);
    EXPECT_EQ(a.resend[0].arg, kPeriodArg) << "参数也要原样重发";
    EXPECT_EQ(a.resend[0].attempt, 1);
    EXPECT_TRUE(t.has(seq)) << "重发后仍在途,继续等应答";
}

// 每次重发从当前时刻重新计算下一个超时窗口。
TEST(CommandTracker, ResendResetsTheClock) {
    CommandTracker t(500ms, 3); // 连续两个窗口验证重发后的新截止时间。
    FakeClock clk;              // 分两段推进第二个 500 ms 窗口。
    t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    clk.advance(500ms);
    ASSERT_EQ(t.tick(clk.now()).resend.size(), 1u);

    clk.advance(200ms); // 距上次重发只有 200 ms。
    EXPECT_TRUE(t.tick(clk.now()).resend.empty()) << "计时没重置会导致连环重发";

    clk.advance(300ms); // 从重发起累计达到 500 ms。
    EXPECT_EQ(t.tick(clk.now()).resend.size(), 1u);
}

// 最后一次重发仍未收到应答时，下一超时窗口将命令转为失败。
TEST(CommandTracker, FailsAfterRetriesExhausted) {
    CommandTracker t(500ms, 3); // 三次重发后在第四个窗口产出失败动作。
    FakeClock clk;              // 逐个推进完整的 500 ms 窗口。
    const uint8_t seq =
        t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now()); // 最终失败动作应引用此序号。

    for (int attempt = 1; attempt <= 3; ++attempt) {
        clk.advance(500ms);
        const auto a = t.tick(clk.now()); // 第 attempt 个超时窗口的动作。
        ASSERT_EQ(a.resend.size(), 1u) << "第 " << attempt << " 次应重发";
        EXPECT_EQ(a.resend[0].attempt, attempt);
        EXPECT_TRUE(a.failed.empty());
    }

    clk.advance(500ms);
    const auto a = t.tick(clk.now()); // 重试耗尽后的失败动作批次。
    EXPECT_TRUE(a.resend.empty()) << "重试次数用尽后不该再发";
    ASSERT_EQ(a.failed.size(), 1u);
    EXPECT_EQ(a.failed[0].seq, seq);
    EXPECT_EQ(a.failed[0].type, EDGE_TYPE_SET_PERIOD);
    EXPECT_FALSE(t.has(seq)) << "判死后应从在途表移除";
}

// 最后一次重发后的等待窗口仍接受正常应答。
TEST(CommandTracker, LateAckBeforeFailureStillMatches) {
    CommandTracker t(500ms, 3); // 已完成三次重发、仍处最终等待窗的跟踪器。
    FakeClock clk;              // 模拟最终应答到达前后的两个时间点。
    const uint8_t seq =
        t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()); // 在最终等待窗内返回 ACK 的命令。

    for (int i = 0; i < 3; ++i) {
        clk.advance(500ms);
        t.tick(clk.now());
    }
    EXPECT_TRUE(t.onAck(seq)) << "第 3 次重发后的应答仍该配对成功";

    clk.advance(500ms);
    EXPECT_TRUE(t.tick(clk.now()).failed.empty()) << "已销账的命令不该再被判死";
}

// 各命令按独立截止时间推进。
TEST(CommandTracker, TracksMultipleCommandsIndependently) {
    CommandTracker t(500ms, 3); // 同时保存两个不同截止时间的命令。
    FakeClock clk;              // 让两次登记相隔 300 ms。

    const uint8_t a_seq = t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now()); // 较早登记、应先到期。
    clk.advance(300ms);
    const uint8_t b_seq = t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()); // 晚 300 ms 登记、尚未到期。

    clk.advance(200ms);                 // a 已满 500 ms，b 只等待 200 ms。
    const auto act = t.tick(clk.now()); // 只能包含 a 的重发动作。

    ASSERT_EQ(act.resend.size(), 1u);
    EXPECT_EQ(act.resend[0].seq, a_seq) << "只有到期的那条该重发";
    EXPECT_TRUE(t.has(b_seq));
}

// 空在途表经过任意时间也不应产生伪造动作或访问无效元素。
TEST(CommandTracker, TickOnEmptyTableDoesNothing) {
    CommandTracker t; // 从未登记命令的空跟踪器。
    FakeClock clk;    // 人为推进十秒以排除时间长度影响。
    clk.advance(10s);

    const auto a = t.tick(clk.now()); // 空表十秒后的动作批次。
    EXPECT_TRUE(a.resend.empty());
    EXPECT_TRUE(a.failed.empty());
}

// 默认超时值由共享协议契约提供。
TEST(CommandTracker, DefaultsComeFromProtocolContract) {
    CommandTracker t; // 默认构造值应直接来自 edge_proto 时序常量。
    FakeClock clk;    // 精确跨越协议默认超时边界。
    t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    clk.advance(std::chrono::milliseconds(EDGE_ACK_TIMEOUT_MS - 1));
    EXPECT_TRUE(t.tick(clk.now()).resend.empty());

    clk.advance(1ms);
    EXPECT_EQ(t.tick(clk.now()).resend.size(), 1u) << "默认时限应为 EDGE_ACK_TIMEOUT_MS";
}
