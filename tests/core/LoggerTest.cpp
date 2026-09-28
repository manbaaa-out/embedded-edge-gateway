// 在子进程内创建日志单例，正常退出时等待异步线程排空，并精确匹配 stderr。
// 父进程不启动日志线程；用例无需依赖刷新周期，也无需为测试增加产品接口。

#include "gateway/core/log/Logger.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

using gateway::kMaxLogLine;
using gateway::LogLevel;
using gateway::Logger;

const std::string kPrefix = "[INFO ] test.cpp:7 ";
const std::string kTruncatedEnding = "... [truncated]\n";

void expectLog(const std::string& file, const std::string& body,
               const std::string& expected) {
    ASSERT_EXIT({
        Logger::log(LogLevel::INFO, file.c_str(), 7, "%s", body.c_str());
        std::exit(0); // 执行单例析构，确保检查的是实际写出的全部字节。
    }, ::testing::ExitedWithCode(0), ::testing::Eq(expected));
}

TEST(LoggerDeathTest, FormatsShortMessage) {
    ASSERT_EXIT({
        Logger::log(LogLevel::INFO, "test.cpp", 7, "value=%d unit=%s", 42, "C");
        std::exit(0);
    }, ::testing::ExitedWithCode(0), ::testing::Eq(kPrefix + "value=42 unit=C\n"));
}

TEST(LoggerDeathTest, KeepsExactlyFullLineWithoutTruncation) {
    const std::string body(kMaxLogLine - kPrefix.size() - 1, 'x');
    expectLog("test.cpp", body, kPrefix + body + "\n");
}

TEST(LoggerDeathTest, MarksBodyOneByteOverLimit) {
    const std::string body(kMaxLogLine - kPrefix.size(), 'x');
    const std::string expected = kPrefix +
        std::string(kMaxLogLine - kPrefix.size() - kTruncatedEnding.size(), 'x') +
        kTruncatedEnding;
    expectLog("test.cpp", body, expected);
}

TEST(LoggerDeathTest, MarksOversizedFormattedResult) {
    const std::string expected = kPrefix +
        std::string(kMaxLogLine - kPrefix.size() - kTruncatedEnding.size(), '0') +
        kTruncatedEnding;
    ASSERT_EXIT({
        Logger::log(LogLevel::INFO, "test.cpp", 7, "%0*d",
                    static_cast<int>(kMaxLogLine * 4), 42);
        std::exit(0);
    }, ::testing::ExitedWithCode(0), ::testing::Eq(expected));
}

TEST(LoggerDeathTest, MarksOversizedPrefixWithEmptyBody) {
    const std::string level = "[INFO ] ";
    const std::string file(kMaxLogLine * 2, 'f');
    const std::string expected = level +
        std::string(kMaxLogLine - level.size() - kTruncatedEnding.size(), 'f') +
        kTruncatedEnding;
    expectLog(file, "", expected);
}

TEST(LoggerDeathTest, KeepsExactlyFullPrefixWithEmptyBody) {
    const std::string level = "[INFO ] ";
    const std::string location = ":7 ";
    const std::string file(kMaxLogLine - level.size() - location.size() - 1, 'f');
    expectLog(file, "", level + file + location + "\n");
}

TEST(LoggerDeathTest, MarksBodyWhenPrefixAlreadyFillsLine) {
    const std::string level = "[INFO ] ";
    const std::string location = ":7 ";
    const std::string file(kMaxLogLine - level.size() - location.size() - 1, 'f');
    const std::string expected = level +
        std::string(kMaxLogLine - level.size() - kTruncatedEnding.size(), 'f') +
        kTruncatedEnding;
    expectLog(file, "x", expected);
}

} // namespace
