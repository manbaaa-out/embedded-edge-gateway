#include "Logger.h"
#include "AsyncLogger.h"

namespace gateway {

LogLevel Logger::level_ = LogLevel::INFO;   // 默认 INFO,DEBUG 不输出

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
    if (lv < level_) return;   // ① 级别过滤

    // ② 在栈上格式化整条日志到一个缓冲区
    char buf[1024];
    int prefix_len = snprintf(buf, sizeof(buf), "[%s] %s:%d ",
                              levelName(lv), file, line);

    va_list ap;
    va_start(ap, fmt);
    int body_len = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    va_end(ap);

    if (body_len < 0) return;   // 格式化失败,丢弃

    int total = prefix_len + body_len;
    if (total >= (int)sizeof(buf)) total = sizeof(buf) - 1;
    buf[total++] = '\n';

    // ③ 异步落盘:走 AsyncLogger 单例(buffer 双缓冲 + 后台 flush 线程)
    AsyncLogger::instance().append(buf, total);
}

} // namespace gateway