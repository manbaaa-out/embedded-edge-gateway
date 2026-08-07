// 在途命令表测试。
//
// 注入假时钟后,整套超时 / 重发 / 判死的时序逻辑可在毫秒内跑完,
// 无需依赖真实串口与真实等待。

#include "gateway/link/CommandTracker.h"

#include <gtest/gtest.h>

using namespace gateway;
using namespace std::chrono_literals;

namespace {

// 假时钟:由测试决定当前时间,不依赖真实流逝
class FakeClock {
public:
    CommandTracker::TimePoint now() const { return base_ + elapsed_; }
    void advance(std::chrono::milliseconds d) { elapsed_ += d; }

private:
    CommandTracker::TimePoint base_    = CommandTracker::Clock::now();
    std::chrono::milliseconds elapsed_ = 0ms;
};

const std::vector<uint8_t> kPeriodArg = {0x07, 0xD0};   // 2000 秒,大端

}  // namespace

TEST(CommandTracker, AssignsIncrementingSeq) {
    CommandTracker t;
    FakeClock      clk;

    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now()), 0);
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 1);
    EXPECT_EQ(t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now()), 2);
    EXPECT_EQ(t.inflightCount(), 3u);
}

// §6.2:seq 是 1 字节,0xFF 之后回绕到 0x00
TEST(CommandTracker, SeqWrapsAroundAt255) {
    CommandTracker t;
    FakeClock      clk;

    for (int i = 0; i < 255; ++i) {
        t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());
        t.onAck(static_cast<uint8_t>(i));   // 立刻销账,避免表被撑满
    }
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 255);
    EXPECT_EQ(t.track(EDGE_TYPE_QUERY_TH, {}, clk.now()), 0) << "0xFF 之后应回绕到 0";
}

TEST(CommandTracker, AckMatchesAndClearsEntry) {
    CommandTracker t;
    FakeClock      clk;

    const uint8_t seq = t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now());
    ASSERT_TRUE(t.has(seq));

    EXPECT_TRUE(t.onAck(seq));
    EXPECT_FALSE(t.has(seq)) << "配对成功即销账";
    EXPECT_EQ(t.inflightCount(), 0u);
}

// 迟到、重复或伪造的应答必须被识别并交给调用方处理,
// 不得误销账到某条无关的在途命令上。
TEST(CommandTracker, UnknownSeqAckIsRejected) {
    CommandTracker t;
    FakeClock      clk;
    const uint8_t  seq = t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    EXPECT_FALSE(t.onAck(static_cast<uint8_t>(seq + 1))) << "不在表里的 seq 应被拒";
    EXPECT_TRUE(t.has(seq)) << "别人的应答不该影响这条";

    EXPECT_TRUE(t.onAck(seq));
    EXPECT_FALSE(t.onAck(seq)) << "重复的应答第二次应被拒";
}

// 结果码不影响「已不在途」这一事实:命令被拒同样构成一次完整应答
TEST(CommandTracker, AckClearsRegardlessOfResultCode) {
    CommandTracker t;
    FakeClock      clk;
    const uint8_t  seq = t.track(EDGE_TYPE_SET_PERIOD, {0x00, 0x00}, clk.now());

    EXPECT_TRUE(t.onAck(seq));   // 调用方侧 rc 可能为 BAD_PARAM
    EXPECT_EQ(t.inflightCount(), 0u);
}

// ---- 超时与重发 ----

TEST(CommandTracker, NoActionBeforeTimeout) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;
    t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now());

    clk.advance(499ms);
    const auto a = t.tick(clk.now());

    EXPECT_TRUE(a.resend.empty()) << "没到时限就不该重发";
    EXPECT_TRUE(a.failed.empty());
}

// §6.2:重发必须复用原 seq。换用新 seq 会使节点侧的幂等判断失效,
// 导致设置类命令被执行两次。
TEST(CommandTracker, ResendReusesSameSeqAndArgs) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;
    const uint8_t  seq = t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now());

    clk.advance(500ms);
    const auto a = t.tick(clk.now());

    ASSERT_EQ(a.resend.size(), 1u);
    EXPECT_EQ(a.resend[0].seq, seq) << "重发必须用同一个 seq(§6.2 幂等)";
    EXPECT_EQ(a.resend[0].type, EDGE_TYPE_SET_PERIOD);
    EXPECT_EQ(a.resend[0].arg, kPeriodArg) << "参数也要原样重发";
    EXPECT_EQ(a.resend[0].attempt, 1);
    EXPECT_TRUE(t.has(seq)) << "重发后仍在途,继续等应答";
}

// 重发后须重置计时,否则下一个 tick 会立即再次重发,瞬间耗尽重试次数
TEST(CommandTracker, ResendResetsTheClock) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;
    t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    clk.advance(500ms);
    ASSERT_EQ(t.tick(clk.now()).resend.size(), 1u);

    clk.advance(200ms);   // 距上次重发才 200ms
    EXPECT_TRUE(t.tick(clk.now()).resend.empty()) << "计时没重置会导致连环重发";

    clk.advance(300ms);   // 累计 500ms
    EXPECT_EQ(t.tick(clk.now()).resend.size(), 1u);
}

// 完整失败链路:发送 → 超时重发 ×3 → 判死
TEST(CommandTracker, FailsAfterRetriesExhausted) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;
    const uint8_t  seq = t.track(EDGE_TYPE_SET_PERIOD, kPeriodArg, clk.now());

    for (int attempt = 1; attempt <= 3; ++attempt) {
        clk.advance(500ms);
        const auto a = t.tick(clk.now());
        ASSERT_EQ(a.resend.size(), 1u) << "第 " << attempt << " 次应重发";
        EXPECT_EQ(a.resend[0].attempt, attempt);
        EXPECT_TRUE(a.failed.empty());
    }

    clk.advance(500ms);
    const auto a = t.tick(clk.now());
    EXPECT_TRUE(a.resend.empty()) << "重试次数用尽后不该再发";
    ASSERT_EQ(a.failed.size(), 1u);
    EXPECT_EQ(a.failed[0].seq, seq);
    EXPECT_EQ(a.failed[0].type, EDGE_TYPE_SET_PERIOD);
    EXPECT_FALSE(t.has(seq)) << "判死后应从在途表移除";
}

// 在最后一次重试与判死之间收到应答,应正常销账而非判死
TEST(CommandTracker, LateAckBeforeFailureStillMatches) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;
    const uint8_t  seq = t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    for (int i = 0; i < 3; ++i) {
        clk.advance(500ms);
        t.tick(clk.now());
    }
    EXPECT_TRUE(t.onAck(seq)) << "第 3 次重发后的应答仍该配对成功";

    clk.advance(500ms);
    EXPECT_TRUE(t.tick(clk.now()).failed.empty()) << "已销账的命令不该再被判死";
}

// 多条命令并存时互不干扰,这是 seq 存在的目的
TEST(CommandTracker, TracksMultipleCommandsIndependently) {
    CommandTracker t(500ms, 3);
    FakeClock      clk;

    const uint8_t a_seq = t.track(EDGE_TYPE_QUERY_LIGHT, {}, clk.now());
    clk.advance(300ms);
    const uint8_t b_seq = t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    clk.advance(200ms);   // a 已过 500ms,b 才 200ms
    const auto act = t.tick(clk.now());

    ASSERT_EQ(act.resend.size(), 1u);
    EXPECT_EQ(act.resend[0].seq, a_seq) << "只有到期的那条该重发";
    EXPECT_TRUE(t.has(b_seq));
}

TEST(CommandTracker, TickOnEmptyTableDoesNothing) {
    CommandTracker t;
    FakeClock      clk;
    clk.advance(10s);

    const auto a = t.tick(clk.now());
    EXPECT_TRUE(a.resend.empty());
    EXPECT_TRUE(a.failed.empty());
}

// 默认参数应直接取自协议契约,而不是另立一套数字
TEST(CommandTracker, DefaultsComeFromProtocolContract) {
    CommandTracker t;   // 默认构造
    FakeClock      clk;
    t.track(EDGE_TYPE_QUERY_TH, {}, clk.now());

    clk.advance(std::chrono::milliseconds(EDGE_ACK_TIMEOUT_MS - 1));
    EXPECT_TRUE(t.tick(clk.now()).resend.empty());

    clk.advance(1ms);
    EXPECT_EQ(t.tick(clk.now()).resend.size(), 1u) << "默认时限应为 EDGE_ACK_TIMEOUT_MS";
}
