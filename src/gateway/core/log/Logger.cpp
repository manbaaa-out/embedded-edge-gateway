// Logger 的同步格式化实现。函数只使用栈内局部状态，不在多个调用线程之间共享缓冲区；
// 生成的前缀、正文和换行一次性交给异步后端，避免多线程日志在行内交错。

#include "gateway/core/log/Logger.h"
#include "gateway/core/log/AsyncLogger.h"

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

    // snprintf/vsnprintf 返回所需长度，计算后续写入位置前必须限制到缓冲区范围。
    char buf[1024]; // 当前调用独占的完整日志行缓冲区，最后一字节可用于换行。
    int ret = snprintf(buf, sizeof(buf), "[%s] %s:%d ", levelName(lv), file, line);
    // ret 是完整前缀所需字符数，不等于发生截断时的实际写入数。
    if (ret < 0) return;

    size_t prefix_len = static_cast<size_t>(ret); // 正文在 buf 中的起始偏移。
    if (prefix_len >= sizeof(buf) - 1) {
        prefix_len = sizeof(buf) - 2;
    }

    va_list ap; // 指向 fmt 后的 printf 实参，仅在本次 vsnprintf 调用中有效。
    va_start(ap, fmt);
    int body_ret = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    // body_ret 是未截断正文所需字符数，用于判断最终行长是否需要夹紧。
    va_end(ap);

    if (body_ret < 0) return;

    size_t total = prefix_len + static_cast<size_t>(body_ret); // 尚未加入换行的逻辑长度。
    if (total >= sizeof(buf)) total = sizeof(buf) - 1;
    buf[total++] = '\n';

    AsyncLogger::instance().append(buf, total);
}

} // namespace gateway
