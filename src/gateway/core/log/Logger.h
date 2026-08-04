// src/log/Logger.h(M3-1 版本,先不动 src/log/CMakeLists.txt)
#pragma once
#include <cstdio>
#include <cstdarg>     // va_list, va_start, va_end

namespace gateway {

enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};

class Logger {
public:
    // 全局最低输出级别(< 这个级别的日志被丢弃)
    static LogLevel level() noexcept { return level_; }
    static void setLevel(LogLevel lv) noexcept { level_ = lv; }

    // 核心写日志函数:格式化 + 写 stderr,一次性写完
    static void log(LogLevel lv, const char* file, int line, const char* fmt, ...);

private:
    static LogLevel level_;
};

} // namespace gateway

// 用宏包一层:自动捕获 __FILE__ / __LINE__,且能在编译期短路(级别低就不展开)
#define LOG_DEBUG(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
