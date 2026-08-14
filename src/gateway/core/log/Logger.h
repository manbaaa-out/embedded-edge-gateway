#pragma once

// 全进程唯一的日志入口:LOG_* 宏 → Logger::log 在栈上格式化 → AsyncLogger 双缓冲。
//
// 分成两层是因为两件事的变化频率不同:「怎么格式化一条日志」几乎不变,
// 「日志往哪去、什么时候落盘」则换过好几次(文件 → stderr → journald)。
// 本层只管前者,后端换掉时这个文件一行不动。

#include <atomic>
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
    // 全局最低输出级别,低于该级别的日志被丢弃。
    //
    // 用 atomic 而非裸变量:reloadConfig 在主线程写它(log_level 属 A 档,SIGHUP 即时
    // 生效),而五条线程都在读。按 C++ 内存模型那是一次数据竞争 —— 实践上 int 大小的
    // 对齐写不会撕裂,但「实践上没事」不该写进一个能用 relaxed 原子零成本表达的地方。
    // relaxed 足够:这里只要求最终可见,不要求与其他内存操作定序。
    static LogLevel level() noexcept { return level_.load(std::memory_order_relaxed); }
    static void setLevel(LogLevel lv) noexcept { level_.store(lv, std::memory_order_relaxed); }

    // 在栈缓冲上格式化完整一行(含结尾换行),整行一次交给后端。
    // 「一次调用 = 一整行」是多线程不交错的前提,详见 .cpp。
    static void log(LogLevel lv, const char* file, int line, const char* fmt, ...);

private:
    static std::atomic<LogLevel> level_;
};

} // namespace gateway

// 包一层宏的两个理由:自动捕获 __FILE__ / __LINE__;级别不足时短路,省掉参数求值。
//
// 注意 fmt 之后至少要给一个变参 —— 无参数时写 LOG_INFO("%s", "text") 而非
// LOG_INFO("text"),否则 ##__VA_ARGS__ 这个 GNU 扩展在 -Wpedantic 下会告警。
#define LOG_DEBUG(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
