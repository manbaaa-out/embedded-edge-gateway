// Logger 的同步格式化实现。函数只使用栈内局部状态，不在多个调用线程之间共享缓冲区；
// 生成的前缀、正文和换行一次性交给异步后端，避免多线程日志在行内交错。

#include "gateway/core/log/Logger.h"
#include "gateway/core/log/AsyncLogger.h"

#include <cstring>

namespace gateway {

std::atomic<LogLevel> Logger::level_{LogLevel::INFO};

/**
 * @param lv 待呈现的严重度枚举。
 * @return 指向静态五字符标签的非拥有指针；未知枚举返回 "?????"。
 */
static const char* levelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?????";
}

void Logger::log(LogLevel lv, const char* file, int line, const char* fmt, ...) {
    if (lv < level_.load(std::memory_order_relaxed)) return;

    constexpr char kTruncatedSuffix[] = "... [truncated]";
    static_assert(sizeof(kTruncatedSuffix) <= kMaxLogLine,
                  "log line must fit the truncation marker and newline");

    // snprintf/vsnprintf 返回所需长度；末尾 NUL 的位置可在提交前改为换行。
    char buf[kMaxLogLine]; // 当前调用独占的完整日志行缓冲区。
    int ret = snprintf(buf, sizeof(buf), "[%s] %s:%d ", levelName(lv), file, line);
    // ret 是完整前缀所需字符数，不等于发生截断时的实际写入数。
    if (ret < 0) return;

    const size_t prefix_len = static_cast<size_t>(ret);
    bool truncated = prefix_len >= sizeof(buf);
    size_t total = prefix_len; // 尚未加入换行的长度；截断时在下方统一裁剪。

    // 前缀本身已超长时不再追加正文，避免使用越界的起始偏移。
    if (!truncated) {
        const size_t remaining = sizeof(buf) - prefix_len;
        va_list ap; // 指向 fmt 后的 printf 实参，仅在本次 vsnprintf 调用中有效。
        va_start(ap, fmt);
        int body_ret = vsnprintf(buf + prefix_len, remaining, fmt, ap);
        va_end(ap);
        if (body_ret < 0) return;

        const size_t body_len = static_cast<size_t>(body_ret);
        truncated = body_len >= remaining;
        total += body_len;
    }

    if (truncated) {
        // 在同一条日志内保留可见标记，仍只向异步后端提交一次。
        constexpr size_t suffix_len = sizeof(kTruncatedSuffix) - 1;
        total = sizeof(buf) - suffix_len - 1;
        std::memcpy(buf + total, kTruncatedSuffix, suffix_len);
        total += suffix_len;
    }
    buf[total++] = '\n';

    AsyncLogger::instance().append(buf, total);
}

} // namespace gateway
