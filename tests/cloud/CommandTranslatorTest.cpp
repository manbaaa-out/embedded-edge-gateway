// CommandTranslator 的纯函数契约测试：输入 MQTT topic/payload，输出不带传输序号的
// 节点命令或可诊断错误，不需要启动 Broker、串口或 GatewayApp。

#include "gateway/cloud/CommandTranslator.h"

#include <gtest/gtest.h>

using namespace gateway;

// 两个查询主题分别映射到对应 TYPE，且不会凭空生成参数字节。
TEST(CommandTranslator, QueryCommandsTakeNoArguments) {
    const auto light = translateCommand("gateway/cmd/query_light", ""); // 光照查询翻译结果。
    ASSERT_TRUE(light.ok) << light.error;
    EXPECT_EQ(light.cmd.type, EDGE_TYPE_QUERY_LIGHT);
    EXPECT_TRUE(light.cmd.arg.empty());

    const auto th = translateCommand("gateway/cmd/query_th", ""); // 温湿度查询翻译结果。
    ASSERT_TRUE(th.ok) << th.error;
    EXPECT_EQ(th.cmd.type, EDGE_TYPE_QUERY_TH);
    EXPECT_TRUE(th.cmd.arg.empty());
}

// 查询命令没有参数语义，意外负载应被忽略而不是透传给节点。
TEST(CommandTranslator, QueryCommandsIgnoreStrayPayload) {
    const auto r = translateCommand("gateway/cmd/query_light", "whatever"); // 携带冗余负载的结果。
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.cmd.arg.empty());
}

// 周期按秒解析，并以协议规定的两字节大端格式发送。
TEST(CommandTranslator, SetPeriodEncodesSecondsAsBigEndian) {
    const auto r = translateCommand("gateway/cmd/set_period", "2000"); // 2000 秒的设置命令。

    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cmd.type, EDGE_TYPE_SET_PERIOD);
    ASSERT_EQ(r.cmd.arg.size(), 2u);
    EXPECT_EQ(r.cmd.arg[0], 0x07) << "高字节在前";
    EXPECT_EQ(r.cmd.arg[1], 0xD0);
    EXPECT_EQ(edge_u16_be_read(r.cmd.arg.data()), 2000);
}

// 协议允许的最小值和 uint16_t 最大值都应通过，避免边界比较少算一端。
TEST(CommandTranslator, SetPeriodAcceptsBoundaryValues) {
    const auto lo = translateCommand("gateway/cmd/set_period", "1"); // 最小合法周期。
    ASSERT_TRUE(lo.ok) << lo.error;
    EXPECT_EQ(edge_u16_be_read(lo.cmd.arg.data()), EDGE_PERIOD_MIN_S);

    const auto hi = translateCommand("gateway/cmd/set_period", "65535"); // 最大合法周期。
    ASSERT_TRUE(hi.ok) << hi.error;
    EXPECT_EQ(edge_u16_be_read(hi.cmd.arg.data()), EDGE_PERIOD_MAX_S);
}

// bad 枚举零、负数、刚越上限和远越上限，每次拒绝都必须携带运维可见原因。
TEST(CommandTranslator, SetPeriodRejectsOutOfRange) {
    for (const char* bad : {"0", "65536", "-1", "999999"}) {
        const auto r = translateCommand("gateway/cmd/set_period", bad); // 当前非法边界的翻译结果。
        EXPECT_FALSE(r.ok) << "'" << bad << "' 不该被接受";
        EXPECT_FALSE(r.error.empty()) << "拒绝必须带原因,便于运维定位";
    }
}

// 设备设置命令要求整个非空白负载均为十进制整数。
TEST(CommandTranslator, SetPeriodRejectsNonNumericPayload) {
    for (const char* bad : {"", "   ", "abc", "12abc", "1.5", "0x10", "1 2"}) {
        const auto r = translateCommand("gateway/cmd/set_period", bad); // 当前非纯整数负载的结果。
        EXPECT_FALSE(r.ok) << "'" << bad << "' 不该被当成合法整数";
    }
}

// 首尾空白不属于数值内容，命令行发布产生的换行也应被接受。
TEST(CommandTranslator, SetPeriodToleratesSurroundingWhitespace) {
    for (const char* good : {"12", " 12", "12\n", "  12  \r\n"}) {
        const auto r = translateCommand("gateway/cmd/set_period", good); // 当前空白形式的解析结果。
        ASSERT_TRUE(r.ok) << "'" << good << "' 应被接受: " << r.error;
        EXPECT_EQ(edge_u16_be_read(r.cmd.arg.data()), 12);
    }
}

// 未知命令不生成节点帧，错误文本包含命令名以便定位云端配置。
TEST(CommandTranslator, UnknownCommandIsRejectedWithReason) {
    const auto r = translateCommand("gateway/cmd/reboot", ""); // 未在命令字典中的主题。
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("reboot"), std::string::npos) << "错误信息应带上命令名";
}

// 命令名来自主题最后一段，前缀不参与翻译。
TEST(CommandTranslator, TakesLastTopicSegmentAsCommandName) {
    EXPECT_TRUE(translateCommand("a/b/c/d/query_th", "").ok);
    EXPECT_TRUE(translateCommand("query_th", "").ok) << "没有斜杠时整个 topic 就是命令名";
    EXPECT_FALSE(translateCommand("gateway/cmd/", "").ok) << "空命令名应被拒";
}

// 序号由发送链路分配，翻译结果只携带命令类型和参数。
TEST(CommandTranslator, ResultCarriesNoSeq) {
    const auto r = translateCommand("gateway/cmd/set_period", "10"); // 只含两字节周期的翻译结果。
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.cmd.arg.size(), 2u) << "只有周期两字节,不含 seq";
}
