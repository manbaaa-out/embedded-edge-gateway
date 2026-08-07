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
    // 全局最低输出级别,低于该级别的日志被丢弃
    static LogLevel level() noexcept { return level_; }
    static void setLevel(LogLevel lv) noexcept { level_ = lv; }

    // 格式化并输出一条日志,整条一次写出以避免多线程交错
    static void log(LogLevel lv, const char* file, int line, const char* fmt, ...);

private:
    static LogLevel level_;
};

} // namespace gateway

// 以宏包一层:自动捕获 __FILE__ / __LINE__,并在级别不足时短路,避免参数求值
#define LOG_DEBUG(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
