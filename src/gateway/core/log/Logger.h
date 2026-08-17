#pragma once

// 进程级日志前端。
//
// LOG_* 宏补充源文件和行号，Logger 负责级别过滤与 printf 风格格式化，AsyncLogger
// 负责输出。每次 Logger::log() 在调用线程的栈上构造完整行后只调用一次 append()，
// 从而把“日志行”作为后端加锁和排序的最小单位。

#include <atomic>
#include <cstdio>
#include <cstdarg>

namespace gateway {

/** 日志严重度；枚举顺序同时定义过滤时的大小关系。 */
enum class LogLevel {
    DEBUG = 0, /**< 开发和细粒度诊断信息。 */
    INFO  = 1, /**< 正常生命周期与重要状态变化。 */
    WARN  = 2, /**< 可恢复异常或降级。 */
    ERROR = 3, /**< 当前操作失败，需要人工关注。 */
};

/** 无实例状态的日志前端；最低级别作为进程级原子配置共享。 */
class Logger {
public:
    /** @return 当前最低输出级别的原子快照。 */
    static LogLevel level() noexcept { return level_.load(std::memory_order_relaxed); }

    /**
     * @brief 更新最低输出级别。
     * @param lv 新阈值；低于它的消息被 Logger::log() 丢弃。
     *
     * relaxed 顺序足够，因为该值不负责发布其他内存状态，只需自身原子可见。
     */
    static void setLevel(LogLevel lv) noexcept { level_.store(lv, std::memory_order_relaxed); }

    /**
     * @brief 格式化并异步提交一条日志。
     * @param lv 本条消息的严重度。
     * @param file 宏传入的源文件路径，写入日志前缀；必须为有效 C 字符串。
     * @param line 宏传入的源代码行号。
     * @param fmt printf 风格格式串，后续参数必须与其匹配。
     *
     * 低于阈值或格式化失败时不输出；完整行上限为 1024 字节，超出部分截断。
     * 异步后端首次初始化或换块时的内存/线程异常会向调用方传播。
     */
    static void log(LogLevel lv, const char* file, int line, const char* fmt, ...);

private:
    static std::atomic<LogLevel> level_; /**< 所有调用线程共享的最低输出级别。 */
};

} // namespace gateway

// 四个宏固定严重度并自动附加 __FILE__/__LINE__。其格式参数会在函数内过滤前求值，
// 因此不应在日志参数中放置依赖“日志已启用”语义的副作用。
#define LOG_DEBUG(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    ::gateway::Logger::log(::gateway::LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
