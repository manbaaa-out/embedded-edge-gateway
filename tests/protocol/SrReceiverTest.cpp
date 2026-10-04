#include "edge_proto/edge_sr.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

namespace {
struct Packet {
    uint8_t type;
    std::vector<uint8_t> p;
};
class Receiver : public testing::Test {
protected:
    edge_sr_node_t node{};
    uint32_t now = 0;
    uint32_t execution_ms = 0;
    std::vector<unsigned> periods;
    std::vector<unsigned> sequences;
    std::vector<Packet> output;
    const edge_sr_hooks_t hooks{[](void* u) { return static_cast<Receiver*>(u)->now; },
                                [](uint8_t seq, uint8_t type, const uint8_t* args, uint8_t len,
                                   uint8_t* out, void* u) -> uint8_t {
                                    auto& self = *static_cast<Receiver*>(u);
                                    self.sequences.push_back(seq);
                                    self.periods.push_back(type == EDGE_TYPE_SET_PERIOD && len == 2
                                                               ? edge_u16_be_read(args)
                                                               : 0);
                                    self.now += self.execution_ms;
                                    out[0] = EDGE_RC_OK;
                                    return 1;
                                },
                                [](uint8_t type, const uint8_t* p, uint8_t n, void* u) {
                                    static_cast<Receiver*>(u)->output.push_back({type, {p, p + n}});
                                }};
    void feed(uint8_t type, const std::vector<uint8_t>& p) {
        edge_sr_node_on_frame(&node, type, p.data(), static_cast<uint8_t>(p.size()), &hooks, this);
    }
    void open(uint64_t session = 100) {
        std::vector<uint8_t> p(8);
        edge_u64_be_write(p.data(), session);
        feed(EDGE_TYPE_SR_OPEN, p);
    }
    std::vector<uint8_t> command(uint8_t seq, uint16_t period = 5, uint64_t session = 100) {
        std::vector<uint8_t> p(12);
        edge_u64_be_write(p.data(), session);
        p[8] = seq;
        p[9] = EDGE_TYPE_SET_PERIOD;
        edge_u16_be_write(p.data() + 10, period);
        return p;
    }
    void data(uint8_t seq, uint16_t period = 5, uint64_t session = 100) {
        feed(EDGE_TYPE_SR_COMMAND, command(seq, period, session));
    }
    void ack(uint8_t seq, uint64_t session = 100) {
        auto p = command(seq, 5, session);
        p.resize(9);
        feed(EDGE_TYPE_SR_RESULT_ACK, p);
    }
    void tick(uint32_t at) {
        now = at;
        edge_sr_node_tick(&node, &hooks, this);
    }
    unsigned count(uint8_t type) const {
        return static_cast<unsigned>(std::count_if(
            output.begin(), output.end(), [type](const Packet& p) { return p.type == type; }));
    }
};

TEST_F(Receiver, ReceivesOutOfOrderButExecutesFiveThenTenOnlyAfterHoleFilled) {
    open();
    output.clear();
    data(1, 10);
    EXPECT_TRUE(periods.empty());
    ASSERT_EQ(output.size(), 1u);
    EXPECT_EQ(output[0].type, EDGE_TYPE_SR_RECEIVED);
    data(1, 10);
    EXPECT_TRUE(periods.empty());
    data(0, 5);
    EXPECT_EQ(periods, std::vector<unsigned>({5, 10}));
    EXPECT_EQ(sequences, std::vector<unsigned>({0, 1}));
    EXPECT_EQ(node.base, 2);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 2u);
    data(0, 5);
    data(1, 10);
    EXPECT_EQ(periods.size(), 2u);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 4u);
}

TEST_F(Receiver, EightFrameReceiveWindowRejectsOutsideRightBoundary) {
    open();
    output.clear();
    data(8);
    data(255);
    EXPECT_TRUE(output.empty());
    for (uint8_t i = 1; i < 8; ++i)
        data(i, i);
    EXPECT_TRUE(periods.empty());
    EXPECT_EQ(count(EDGE_TYPE_SR_RECEIVED), 7u);
    data(0, 99);
    EXPECT_EQ(sequences, std::vector<unsigned>({0, 1, 2, 3, 4, 5, 6, 7}));
    data(8, 8);
    EXPECT_EQ(node.base, 9);
}

TEST_F(Receiver, NewSessionCancelsBufferedFollowersAndRejectsLateOldDataOrOpen) {
    open();
    data(1, 10);
    open(101);
    data(0, 5);
    data(1, 10);
    open(100);
    EXPECT_TRUE(periods.empty());
    EXPECT_EQ(node.session, 101u);
    data(0, 20, 101);
    EXPECT_EQ(periods, std::vector<unsigned>({20}));
    EXPECT_EQ(output.back().type, EDGE_TYPE_SR_RESULT);
}

TEST_F(Receiver, DuplicateOpenDoesNotClearBufferOrReexecuteCommands) {
    open();
    data(1, 10);
    open();
    data(0, 5);
    open();
    data(0, 5);
    EXPECT_EQ(periods, std::vector<unsigned>({5, 10}));
    EXPECT_EQ(node.base, 2);
}

TEST_F(Receiver, HoleExpiresAndSameSessionCannotReactivateIt) {
    open();
    data(1, 10);
    tick(3999);
    EXPECT_EQ(node.active, 1);
    tick(4000);
    EXPECT_EQ(node.active, 0);
    data(0, 5);
    open();
    EXPECT_EQ(node.active, 0);
    EXPECT_EQ(output.back().p[16], EDGE_RC_BAD_PARAM);
    EXPECT_TRUE(periods.empty());
    open(101);
    data(0, 20, 101);
    EXPECT_EQ(periods, std::vector<unsigned>({20}));
}

TEST_F(Receiver, ArrivalAtGapDeadlineCannotExecuteExpiredFollowers) {
    open();
    data(1, 10);
    now = 4000;
    data(0, 5);
    EXPECT_TRUE(periods.empty());
    EXPECT_EQ(node.active, 0);
}

TEST_F(Receiver, ResultResendsIndependentlyAndStopsAfterAckOrRetryBudget) {
    open();
    data(0);
    output.clear();
    tick(499);
    EXPECT_TRUE(output.empty());
    tick(500);
    tick(1000);
    tick(1500);
    tick(2000);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 3u);
    EXPECT_EQ(periods.size(), 1u);
    ack(0);
    output.clear();
    tick(3000);
    data(0);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 0u);
    EXPECT_EQ(periods.size(), 1u);
}

TEST_F(Receiver, ResultTtlIsFixedAndExpiryNeverPermitsReexecution) {
    open();
    data(0);
    const auto original = output.back().p;
    now = 4999;
    data(0);
    EXPECT_EQ(output.back().p, original);
    output.clear();
    tick(5000);
    data(0);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 0u);
    EXPECT_EQ(periods.size(), 1u);
    EXPECT_EQ(node.base, 1);
}

TEST_F(Receiver, ResultTtlStartsAfterExecutionAndContiguousWorkHasNoGapTimeout) {
    open();
    execution_ms = 700;
    for (uint8_t i = 1; i < 8; ++i)
        data(i, i);
    data(0, 5);
    EXPECT_EQ(periods.size(), 8u);
    EXPECT_EQ(node.active, 1);
    EXPECT_EQ(now, 5600u);
    output.clear();
    now = 5699;
    data(0, 5);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 1u);
    tick(5700);
    output.clear();
    data(0, 5);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 0u);
    EXPECT_EQ(periods.size(), 8u);
}

TEST_F(Receiver, FullResultPoolRefusesNewCommandsWithoutEvictingOldResults) {
    open();
    for (uint8_t i = 0; i < 16; ++i)
        data(i, i);
    output.clear();
    data(16, 16);
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(node.base, 16);
    data(0, 0);
    EXPECT_EQ(count(EDGE_TYPE_SR_RESULT), 1u);
    ack(0);
    data(16, 16);
    EXPECT_EQ(node.base, 17);
    EXPECT_EQ(periods.size(), 17u);
}

TEST_F(Receiver, ConflictingPendingAndExecutedDuplicatesAreRejected) {
    open();
    data(1, 10);
    output.clear();
    data(1, 5);
    EXPECT_TRUE(output.empty());
    data(0, 5);
    output.clear();
    data(1, 5);
    EXPECT_TRUE(output.empty());
    auto p = command(1, 10);
    p[9] = EDGE_TYPE_QUERY_LIGHT;
    feed(EDGE_TYPE_SR_COMMAND, p);
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(periods, std::vector<unsigned>({5, 10}));
}

TEST_F(Receiver, PendingResultAckCannotDeleteUnexecutedCommand) {
    open();
    data(1, 10);
    ack(1);
    data(0, 5);
    EXPECT_EQ(periods, std::vector<unsigned>({5, 10}));
}

TEST_F(Receiver, InvalidLegacyFramesAndPreHandshakeDataCannotExecute) {
    data(0);
    feed(EDGE_TYPE_SET_PERIOD, {0, 0, 5});
    EXPECT_TRUE(output.empty());
    open();
    output.clear();
    for (unsigned n = 0; n < 10; ++n) {
        auto p = command(0);
        p.resize(n);
        feed(EDGE_TYPE_SR_COMMAND, p);
    }
    auto p = command(0);
    p.resize(64);
    feed(EDGE_TYPE_SR_COMMAND, p);
    feed(EDGE_TYPE_SET_PERIOD, {0, 0, 5});
    EXPECT_TRUE(periods.empty());
    EXPECT_TRUE(output.empty());
}

TEST_F(Receiver, ClockWrapPreservesGapAndCacheDeadlines) {
    now = std::numeric_limits<uint32_t>::max() - 100;
    open();
    data(0);
    data(2);
    tick(398u);
    EXPECT_EQ(node.active, 1);
    tick(3898u);
    EXPECT_EQ(node.active, 1);
    tick(3899u);
    EXPECT_EQ(node.active, 0);
    EXPECT_EQ(periods.size(), 1u);
}

TEST_F(Receiver, SequenceSpaceCannotWrapInsideSession) {
    open();
    for (unsigned i = 0; i < 256; ++i) {
        data(static_cast<uint8_t>(i), static_cast<uint16_t>(i + 1));
        ack(static_cast<uint8_t>(i));
    }
    EXPECT_EQ(node.base, 256);
    data(0, 1);
    data(255, 256);
    EXPECT_EQ(periods.size(), 256u);
    open(101);
    data(0, 300, 101);
    EXPECT_EQ(periods.back(), 300u);
    EXPECT_EQ(periods.size(), 257u);
}

TEST_F(Receiver, ShuffledWindowsWithDuplicatesExecuteEveryCommandInOrder) {
    open();
    std::mt19937 rng(42);
    for (unsigned base = 0; base < 256; base += 8) {
        std::vector<unsigned> order(8);
        std::iota(order.begin(), order.end(), base);
        std::shuffle(order.begin(), order.end(), rng);
        for (unsigned i : order) {
            data(static_cast<uint8_t>(i));
            data(static_cast<uint8_t>(i));
        }
        for (unsigned i : order)
            ack(static_cast<uint8_t>(i));
    }
    std::vector<unsigned> expected(256);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(sequences, expected);
}
} // namespace
