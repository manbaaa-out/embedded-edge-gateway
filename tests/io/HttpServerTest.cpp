// clampReportN 的纯函数边界测试。第一个参数是 URL 查询值文本，第二个参数是配置的
// report_n；返回值最终进入 SQLite LIMIT，因此任何路径都必须落在 [1, kMaxReportN]。

#include "gateway/io/http/HttpServer.h"

#include <gtest/gtest.h>

using gateway::clampReportN;
using gateway::kMaxReportN;

// 查询参数缺失时直接使用配置默认值，确保运行期配置确实影响接口行为。
TEST(ClampReportN, AbsentParamFallsBackToConfiguredDefault) {
    EXPECT_EQ(clampReportN("", 10), 10);
    EXPECT_EQ(clampReportN("", 50), 50) << "配置改成 50 就该是 50,不能永远是 10";
}

// 合法请求参数优先于配置，两个端点和中间值均保持原值。
TEST(ClampReportN, ValidParamWins) {
    EXPECT_EQ(clampReportN("1", 10), 1);
    EXPECT_EQ(clampReportN("200", 10), 200);
    EXPECT_EQ(clampReportN("1000", 10), kMaxReportN) << "上限本身是合法值";
}

// 非法请求值回退到配置值，不直接参与查询。
TEST(ClampReportN, OutOfRangeAndGarbageFallBack) {
    EXPECT_EQ(clampReportN("0", 10), 10);
    EXPECT_EQ(clampReportN("-5", 10), 10);
    EXPECT_EQ(clampReportN("1001", 10), 10) << "刚过上限";
    EXPECT_EQ(clampReportN("99999999999999999999", 10), 10) << "stoi 抛 out_of_range";
    EXPECT_EQ(clampReportN("abc", 10), 10);
    EXPECT_EQ(clampReportN("  ", 10), 10);
}

// 当前接口保留 stoi 接受数字前缀的行为；该用例明确锁定兼容性边界。
TEST(ClampReportN, LeadingNumberIsAcceptedAsBefore) {
    EXPECT_EQ(clampReportN("12abc", 10), 12);
}

// 防御性夹紧配置值，确保所有回退路径仍满足查询上限。
TEST(ClampReportN, ConfiguredDefaultIsItselfClamped) {
    EXPECT_EQ(clampReportN("", 5000), kMaxReportN);
    EXPECT_EQ(clampReportN("abc", 5000), kMaxReportN) << "回落路径同样要夹紧";
    EXPECT_EQ(clampReportN("", 0), 1) << "校验拦得住 0,此处仍不放行非法值";
    EXPECT_EQ(clampReportN("", -1), 1);
}
