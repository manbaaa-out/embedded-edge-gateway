// /api/data 点数归一的边界测试。
//
// HTTP 服务本身要真跑起来才能验(见 scripts/e2e_verify.sh),但这条读路径上
// 唯一不碰 I/O 的判断可以单独钉死:请求带来的 n 与配置给的默认值,
// 谁在什么情况下胜出、越界时回落到哪。

#include "gateway/io/http/HttpServer.h"

#include <gtest/gtest.h>

using gateway::clampReportN;
using gateway::kMaxReportN;

// 未带 n 参数时用配置里的 report_n —— 这正是此前写死 10、配置改了不生效的那处
TEST(ClampReportN, AbsentParamFallsBackToConfiguredDefault) {
    EXPECT_EQ(clampReportN("", 10), 10);
    EXPECT_EQ(clampReportN("", 50), 50) << "配置改成 50 就该是 50,不能永远是 10";
}

// 合法范围内的请求参数优先于配置默认值
TEST(ClampReportN, ValidParamWins) {
    EXPECT_EQ(clampReportN("1", 10), 1);
    EXPECT_EQ(clampReportN("200", 10), 200);
    EXPECT_EQ(clampReportN("1000", 10), kMaxReportN) << "上限本身是合法值";
}

// 越界与非法输入一律回落到默认值,不得把非法值带进 SQL
TEST(ClampReportN, OutOfRangeAndGarbageFallBack) {
    EXPECT_EQ(clampReportN("0", 10), 10);
    EXPECT_EQ(clampReportN("-5", 10), 10);
    EXPECT_EQ(clampReportN("1001", 10), 10) << "刚过上限";
    EXPECT_EQ(clampReportN("99999999999999999999", 10), 10) << "stoi 抛 out_of_range";
    EXPECT_EQ(clampReportN("abc", 10), 10);
    EXPECT_EQ(clampReportN("  ", 10), 10);
}

// stoi 的宽松解析是既有行为,读接口上无害,明确固定下来以免被当成 bug 改掉
TEST(ClampReportN, LeadingNumberIsAcceptedAsBefore) {
    EXPECT_EQ(clampReportN("12abc", 10), 12);
}

// 配置只校验 report_n > 0,故默认值自身也可能越界,必须一并夹紧 ——
// 否则配置文件就绕过了这里的上限,一次查询能把整张表拖进内存
TEST(ClampReportN, ConfiguredDefaultIsItselfClamped) {
    EXPECT_EQ(clampReportN("", 5000), kMaxReportN);
    EXPECT_EQ(clampReportN("abc", 5000), kMaxReportN) << "回落路径同样要夹紧";
    EXPECT_EQ(clampReportN("", 0), 1) << "校验拦得住 0,此处仍不放行非法值";
    EXPECT_EQ(clampReportN("", -1), 1);
}
