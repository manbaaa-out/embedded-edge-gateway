// MQTT 下行命令翻译测试。translateCommand 是纯函数,无需连接 broker 即可验证。

#include "gateway/cloud/CommandTranslator.h"

#include <gtest/gtest.h>

using namespace gateway;

TEST(CommandTranslator, QueryCommandsTakeNoArguments) {
    const auto light = translateCommand("gateway/cmd/query_light", "");
    ASSERT_TRUE(light.ok) << light.error;
    EXPECT_EQ(light.cmd.type, EDGE_TYPE_QUERY_LIGHT);
    EXPECT_TRUE(light.cmd.arg.empty());

    const auto th = translateCommand("gateway/cmd/query_th", "");
    ASSERT_TRUE(th.ok) << th.error;
    EXPECT_EQ(th.cmd.type, EDGE_TYPE_QUERY_TH);
    EXPECT_TRUE(th.cmd.arg.empty());
}

// 查询类命令携带 payload 时参数被忽略,不应因此失败
TEST(CommandTranslator, QueryCommandsIgnoreStrayPayload) {
    const auto r = translateCommand("gateway/cmd/query_light", "whatever");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.cmd.arg.empty());
}

// 周期单位为秒,编码为 2 字节大端,与节点侧的读法一致
TEST(CommandTranslator, SetPeriodEncodesSecondsAsBigEndian) {
    const auto r = translateCommand("gateway/cmd/set_period", "2000");

    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cmd.type, EDGE_TYPE_SET_PERIOD);
    ASSERT_EQ(r.cmd.arg.size(), 2u);
    EXPECT_EQ(r.cmd.arg[0], 0x07) << "高字节在前";
    EXPECT_EQ(r.cmd.arg[1], 0xD0);
    EXPECT_EQ(edge_u16_be_read(r.cmd.arg.data()), 2000);
}

TEST(CommandTranslator, SetPeriodAcceptsBoundaryValues) {
    const auto lo = translateCommand("gateway/cmd/set_period", "1");
    ASSERT_TRUE(lo.ok) << lo.error;
    EXPECT_EQ(edge_u16_be_read(lo.cmd.arg.data()), EDGE_PERIOD_MIN_S);

    const auto hi = translateCommand("gateway/cmd/set_period", "65535");
    ASSERT_TRUE(hi.ok) << hi.error;
    EXPECT_EQ(edge_u16_be_read(hi.cmd.arg.data()), EDGE_PERIOD_MAX_S);
}

// §6.3:周期 0 为非法参数。在网关侧拦截可省去一次到节点的往返。
TEST(CommandTranslator, SetPeriodRejectsOutOfRange) {
    for (const char* bad : {"0", "65536", "-1", "999999"}) {
        const auto r = translateCommand("gateway/cmd/set_period", bad);
        EXPECT_FALSE(r.ok) << "'" << bad << "' 不该被接受";
        EXPECT_FALSE(r.error.empty()) << "拒绝必须带原因,便于运维定位";
    }
}

// 严格解析:std::stoi 会把 "12abc" 解析成 12,而该参数将写入设备,不应容忍。
TEST(CommandTranslator, SetPeriodRejectsNonNumericPayload) {
    for (const char* bad : {"", "   ", "abc", "12abc", "1.5", "0x10", "1 2"}) {
        const auto r = translateCommand("gateway/cmd/set_period", bad);
        EXPECT_FALSE(r.ok) << "'" << bad << "' 不该被当成合法整数";
    }
}

// 但两端空白必须容忍:MQTT payload 常来自 shell,尾随换行是常态。
TEST(CommandTranslator, SetPeriodToleratesSurroundingWhitespace) {
    for (const char* good : {"12", " 12", "12\n", "  12  \r\n"}) {
        const auto r = translateCommand("gateway/cmd/set_period", good);
        ASSERT_TRUE(r.ok) << "'" << good << "' 应被接受: " << r.error;
        EXPECT_EQ(edge_u16_be_read(r.cmd.arg.data()), 12);
    }
}

TEST(CommandTranslator, UnknownCommandIsRejectedWithReason) {
    const auto r = translateCommand("gateway/cmd/reboot", "");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("reboot"), std::string::npos) << "错误信息应带上命令名";
}

// 只取 topic 最后一段,前缀可为任意层级
TEST(CommandTranslator, TakesLastTopicSegmentAsCommandName) {
    EXPECT_TRUE(translateCommand("a/b/c/d/query_th", "").ok);
    EXPECT_TRUE(translateCommand("query_th", "").ok) << "没有斜杠时整个 topic 就是命令名";
    EXPECT_FALSE(translateCommand("gateway/cmd/", "").ok) << "空命令名应被拒";
}

// 翻译结果不含 seq:seq 由 CommandTracker 在发送时分配,
// 以便重发复用同一个值(§6.2)。
TEST(CommandTranslator, ResultCarriesNoSeq) {
    const auto r = translateCommand("gateway/cmd/set_period", "10");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.cmd.arg.size(), 2u) << "只有周期两字节,不含 seq";
}
