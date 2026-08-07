#include "gateway/core/log/Logger.h"
#include "gateway/core/log/AsyncLogger.h"

namespace gateway {

LogLevel Logger::level_ = LogLevel::INFO;   // 默认不输出 DEBUG

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
    if (lv < level_) return;

    // 在栈缓冲上格式化整条日志。
    // snprintf 返回的是「本应写入的长度」而非实际写入的长度:前缀含文件路径,
    // 路径够长时 prefix_len 会超过 sizeof(buf),此时 buf + prefix_len 已越界,
    // 且 sizeof(buf) - prefix_len 作为 size_t 会下溢成巨大值,vsnprintf 将据此
    // 向越界地址写入。故必须先夹紧再使用。
    char buf[1024];
    int  ret = snprintf(buf, sizeof(buf), "[%s] %s:%d ", levelName(lv), file, line);
    if (ret < 0) return;                                  // 格式化失败,丢弃

    size_t prefix_len = static_cast<size_t>(ret);
    if (prefix_len >= sizeof(buf) - 1) {
        prefix_len = sizeof(buf) - 2;                     // 为换行与 '\0' 各留一字节
    }

    va_list ap;
    va_start(ap, fmt);
    int body_ret = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    va_end(ap);

    if (body_ret < 0) return;   // 格式化失败,丢弃

    size_t total = prefix_len + static_cast<size_t>(body_ret);   // body_ret 同为「本应写入」
    if (total >= sizeof(buf)) total = sizeof(buf) - 1;
    buf[total++] = '\n';

    // 异步落盘:AsyncLogger 单例内部为双缓冲 + 后台 flush 线程
    AsyncLogger::instance().append(buf, total);
}

} // namespace gateway