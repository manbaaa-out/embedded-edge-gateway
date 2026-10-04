#include "gateway/link/CommandTracker.h"

#include <gtest/gtest.h>
#include <limits>

using namespace gateway;
using namespace std::chrono_literals;

namespace {
std::vector<uint8_t> prefix(uint64_t session, uint8_t seq) {
    std::vector<uint8_t> p(9);
    edge_u64_be_write(p.data(), session);
    p[8] = seq;
    return p;
}
std::vector<uint8_t> opened(uint64_t requested, uint64_t high, uint8_t rc = EDGE_RC_OK) {
    std::vector<uint8_t> p(17);
    edge_u64_be_write(p.data(), requested);
    edge_u64_be_write(p.data() + 8, high);
    p[16] = rc;
    return p;
}
std::vector<uint8_t> result(uint64_t session, uint8_t seq, uint8_t command = EDGE_TYPE_SET_PERIOD,
                            std::vector<uint8_t> data = {EDGE_RC_OK}) {
    auto p = prefix(session, seq);
    p.push_back(command);
    p.insert(p.end(), data.begin(), data.end());
    return p;
}
class Sender : public testing::Test {
protected:
    CommandTracker tracker;
    CommandTracker::TimePoint now{};
    void ready() {
        const auto a = tracker.start(100, now);
        ASSERT_EQ(a.send.size(), 1u);
        tracker.onWritten(a.send[0], true, now);
        tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(100, 100), now);
        ASSERT_EQ(tracker.state(), CommandTracker::State::Ready);
    }
    CommandSend send(uint16_t period = 5) {
        auto s =
            tracker.submit(EDGE_TYPE_SET_PERIOD,
                           {static_cast<uint8_t>(period >> 8), static_cast<uint8_t>(period)}, now);
        EXPECT_TRUE(s.has_value());
        if (!s) return {};
        tracker.onWritten(*s, true, now);
        return *s;
    }
};

TEST_F(Sender, HandshakeRequiredAndDuplicateOpenAckDoesNotResetWindow) {
    EXPECT_FALSE(tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now));
    auto open = tracker.start(100, now).send.at(0);
    EXPECT_FALSE(tracker.canSubmit());
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(100, 100), now); // 尚未写出不能确认。
    EXPECT_EQ(tracker.state(), CommandTracker::State::Opening);
    tracker.onWritten(open, true, now);
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(100, 100), now);
    send();
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(100, 100), now);
    EXPECT_EQ(tracker.nextSeq(), 1);
    EXPECT_EQ(tracker.inflightCount(), 1u);
}

TEST_F(Sender, SelectiveAcksDoNotSlidePastAHoleOrReportExecution) {
    ready();
    for (int i = 0; i < 8; ++i)
        send();
    for (uint8_t seq = 1; seq < 8; ++seq) {
        const auto a = tracker.onFrame(EDGE_TYPE_SR_RECEIVED, prefix(100, seq), now);
        EXPECT_TRUE(a.finished.empty());
    }
    EXPECT_EQ(tracker.sendBase(), 0);
    EXPECT_FALSE(tracker.canSubmit());
    auto retries = tracker.tick(now + 500ms);
    ASSERT_EQ(retries.send.size(), 1u);
    EXPECT_EQ(retries.send[0].seq, 0);
    EXPECT_EQ(retries.send[0].attempt, 1u);
    tracker.onFrame(EDGE_TYPE_SR_RECEIVED, prefix(100, 0), now + 501ms);
    EXPECT_EQ(tracker.sendBase(), 8);
    EXPECT_FALSE(tracker.canSubmit()); // 尚未得到任何执行结果，业务在途也有界。
    EXPECT_FALSE(tracker.canSend(retries.send[0], now + 501ms));
    tracker.onWritten(retries.send[0], false, now + 501ms);
    EXPECT_EQ(tracker.state(), CommandTracker::State::Ready);
    EXPECT_TRUE(tracker.tick(now + 2s).send.empty());
    tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now + 2s);
    EXPECT_TRUE(tracker.canSubmit());
    EXPECT_EQ(send().seq, 8);
}

TEST_F(Sender, LaterResultsDoNotFreeSequenceSpanAcrossMissingBase) {
    ready();
    for (int i = 0; i < 8; ++i)
        send();
    for (uint8_t seq = 1; seq < 8; ++seq)
        tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, seq), now);
    EXPECT_EQ(tracker.inflightCount(), 1u);
    EXPECT_FALSE(tracker.canSubmit());
    tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now);
    EXPECT_EQ(tracker.sendBase(), 8);
    EXPECT_TRUE(tracker.canSubmit());
}

TEST_F(Sender, ResultImpliesReceiptAndDuplicatesOnlyRepeatResultAck) {
    ready();
    send();
    auto first = tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now);
    ASSERT_EQ(first.finished.size(), 1u);
    EXPECT_TRUE(first.finished[0].error.empty());
    ASSERT_EQ(first.send.size(), 1u);
    EXPECT_EQ(first.send[0].type, EDGE_TYPE_SR_RESULT_ACK);
    EXPECT_EQ(tracker.sendBase(), 1);
    auto dup = tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now + 1s);
    EXPECT_TRUE(dup.finished.empty());
    ASSERT_EQ(dup.send.size(), 1u);
    EXPECT_EQ(dup.send[0].payload, first.send[0].payload);
    EXPECT_TRUE(tracker.tick(now + 1s).send.empty());
}

TEST_F(Sender, RejectsWrongSessionTypeLengthCodeAndUnsentResults) {
    ready();
    auto s = tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now).value();
    EXPECT_TRUE(tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now).finished.empty());
    tracker.onWritten(s, true, now);
    for (const auto& p :
         {result(99, 0), result(100, 1), result(100, 0, EDGE_TYPE_QUERY_LIGHT, {0, 0, 1}),
          result(100, 0, EDGE_TYPE_SET_PERIOD, {0, 1}), result(100, 0, EDGE_TYPE_SET_PERIOD, {99}),
          std::vector<uint8_t>{}})
        EXPECT_TRUE(tracker.onFrame(EDGE_TYPE_SR_RESULT, p, now).finished.empty());
    EXPECT_EQ(tracker.inflightCount(), 1u);
    const auto done = tracker.onFrame(
        EDGE_TYPE_SR_RESULT, result(100, 0, EDGE_TYPE_SET_PERIOD, {EDGE_RC_BAD_PARAM}), now);
    ASSERT_EQ(done.finished.size(), 1u);
    EXPECT_EQ(done.finished[0].result, std::vector<uint8_t>({EDGE_RC_BAD_PARAM}));
}

TEST_F(Sender, RetriesStartAfterFullWriteAndQueueOnlyOnce) {
    ready();
    auto s = tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now).value();
    EXPECT_TRUE(tracker.tick(now + 900ms).send.empty());
    tracker.onWritten(s, true, now + 900ms);
    EXPECT_TRUE(tracker.tick(now + 1399ms).send.empty());
    auto r = tracker.tick(now + 1400ms).send.at(0);
    EXPECT_EQ(r.payload, s.payload);
    EXPECT_TRUE(tracker.tick(now + 2300ms).send.empty());
    tracker.onWritten(r, true, now + 2300ms);
    EXPECT_TRUE(tracker.tick(now + 2799ms).send.empty());
    EXPECT_EQ(tracker.tick(now + 2800ms).send.size(), 1u);
}

TEST_F(Sender, ExhaustedHoleAbortsBufferedFollowersAndRequiresNewHandshake) {
    ready();
    const auto old = send();
    send(10);
    tracker.onFrame(EDGE_TYPE_SR_RECEIVED, prefix(100, 1), now);
    for (int i = 1; i <= 3; ++i) {
        const auto at = now + i * 500ms;
        auto a = tracker.tick(at);
        ASSERT_EQ(a.send.size(), 1u);
        EXPECT_EQ(a.send[0].seq, 0);
        tracker.onWritten(a.send[0], true, at);
    }
    auto fail = tracker.tick(now + 2s);
    ASSERT_EQ(fail.finished.size(), 2u);
    EXPECT_EQ(fail.finished[0].error, "timeout");
    EXPECT_EQ(fail.finished[1].error, "session_aborted");
    EXPECT_EQ(tracker.state(), CommandTracker::State::Opening);
    EXPECT_FALSE(tracker.canSubmit());
    ASSERT_EQ(fail.send.size(), 1u);
    EXPECT_EQ(fail.send[0].type, EDGE_TYPE_SR_OPEN);
    EXPECT_EQ(tracker.session(), 101u);
    EXPECT_FALSE(tracker.canSend(old, now + 2s));
    tracker.onWritten(old, true, now + 2s);
    EXPECT_TRUE(tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now + 2s).finished.empty());
    tracker.onWritten(fail.send[0], true, now + 2s);
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(101, 101), now + 2s);
    EXPECT_EQ(send(20).seq, 0);
    EXPECT_EQ(tracker.inflightCount(), 1u);
}

TEST_F(Sender, InitialAndRetryQueueHaveAbsoluteDeadline) {
    ready();
    auto s = tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now).value();
    EXPECT_TRUE(tracker.canSend(s, now + 3999ms));
    EXPECT_FALSE(tracker.canSend(s, now + 4s));
    auto a = tracker.onWritten(s, false, now + 4s);
    ASSERT_EQ(a.finished.size(), 1u);
    EXPECT_EQ(a.finished[0].error, "timeout");
    EXPECT_TRUE(tracker.tick(now + 4s).finished.empty());
}

TEST_F(Sender, ReceivedCommandWaitsForResultThenClosesSession) {
    ready();
    send();
    tracker.onFrame(EDGE_TYPE_SR_RECEIVED, prefix(100, 0), now);
    EXPECT_TRUE(tracker.tick(now + 4s).send.empty());
    EXPECT_TRUE(tracker.tick(now + 9999ms).finished.empty());
    auto a = tracker.tick(now + 10s);
    ASSERT_EQ(a.finished.size(), 1u);
    EXPECT_EQ(a.finished[0].error, "timeout");
    EXPECT_EQ(tracker.state(), CommandTracker::State::Opening);
}

TEST_F(Sender, LocalSendFailureCancelsAllOutstandingCommands) {
    ready();
    auto s = tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now).value();
    send(10);
    const auto a = tracker.onWritten(s, false, now);
    ASSERT_EQ(a.finished.size(), 2u);
    EXPECT_EQ(a.finished[0].error, "send_failed");
    EXPECT_EQ(a.finished[1].error, "session_aborted");
}

TEST_F(Sender, OpenRetriesAreBoundedAndQueuedCommandsCannotBypassFailure) {
    auto s = tracker.start(100, now).send.at(0);
    tracker.onWritten(s, true, now);
    for (int i = 1; i <= 3; ++i) {
        auto a = tracker.tick(now + i * 500ms);
        ASSERT_EQ(a.send.size(), 1u);
        EXPECT_EQ(a.send[0].payload, s.payload);
        tracker.onWritten(a.send[0], true, now + i * 500ms);
    }
    EXPECT_TRUE(tracker.tick(now + 2s).send.empty());
    EXPECT_EQ(tracker.state(), CommandTracker::State::Failed);
    EXPECT_FALSE(tracker.submit(EDGE_TYPE_SET_PERIOD, {0, 5}, now + 2s));
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(100, 100), now + 2s);
    EXPECT_EQ(tracker.state(), CommandTracker::State::Failed);
}

TEST_F(Sender, OpenBackpressureIsBounded) {
    auto s = tracker.start(100, now).send.at(0);
    EXPECT_TRUE(tracker.tick(now + 3999ms).send.empty());
    EXPECT_FALSE(tracker.canSend(s, now + 4s));
    tracker.tick(now + 4s);
    EXPECT_EQ(tracker.state(), CommandTracker::State::Failed);
}

TEST_F(Sender, RejectedOpenAdvancesHighWaterWithoutResettingRetryBudget) {
    auto s = tracker.start(1, now).send.at(0);
    for (uint64_t i = 1; i <= 4; ++i) {
        tracker.onWritten(s, true, now);
        auto a = tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK,
                                 opened(tracker.session(), i * 100, EDGE_RC_BAD_PARAM), now);
        if (i < 4) {
            ASSERT_EQ(a.send.size(), 1u);
            s = a.send[0];
            EXPECT_EQ(s.session, i * 100 + 1);
        } else {
            EXPECT_TRUE(a.send.empty());
            EXPECT_EQ(tracker.state(), CommandTracker::State::Failed);
        }
    }
}

TEST_F(Sender, SequenceRolloverDrainsThenFencesDelayedOldFrames) {
    ready();
    CommandSend last;
    for (unsigned i = 0; i < 256; ++i) {
        last = send();
        EXPECT_EQ(last.seq, i);
        if (i == 255) {
            EXPECT_FALSE(tracker.canSubmit());
        }
        auto a = tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, last.seq), now);
        ASSERT_EQ(a.finished.size(), 1u);
        if (i < 255) {
            EXPECT_EQ(tracker.state(), CommandTracker::State::Ready);
        } else {
            EXPECT_EQ(tracker.state(), CommandTracker::State::Opening);
            ASSERT_EQ(a.send.back().type, EDGE_TYPE_SR_OPEN);
            tracker.onWritten(a.send.back(), true, now);
        }
    }
    tracker.onFrame(EDGE_TYPE_SR_OPEN_ACK, opened(101, 101), now);
    EXPECT_EQ(send().seq, 0);
    EXPECT_TRUE(tracker.onFrame(EDGE_TYPE_SR_RESULT, result(100, 0), now).finished.empty());
    tracker.onWritten(last, false, now);
    EXPECT_EQ(tracker.inflightCount(), 1u);
}

TEST_F(Sender, SessionCannotWrapOrGoBackwards) {
    auto a = tracker.start(std::numeric_limits<uint64_t>::max(), now);
    ASSERT_EQ(a.send.size(), 1u);
    tracker.start(0, now);
    EXPECT_EQ(tracker.state(), CommandTracker::State::Failed);
    EXPECT_EQ(tracker.session(), std::numeric_limits<uint64_t>::max());
}
} // namespace
