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
    //   命门:snprintf 返回的是【本应写入的长度】,不是实际写入的长度。
    //   前缀含文件路径,路径够长时 prefix_len 会超过 sizeof(buf) —— 此时
    //   buf + prefix_len 已越界,而 sizeof(buf) - prefix_len 作为 size_t 会
    //   下溢成天文数字,vsnprintf 拿着它往越界地址写。必须先夹紧再用。
    char buf[1024];
    int  ret = snprintf(buf, sizeof(buf), "[%s] %s:%d ", levelName(lv), file, line);
    if (ret < 0) return;                                  // 格式化失败,丢弃

    size_t prefix_len = static_cast<size_t>(ret);
    if (prefix_len >= sizeof(buf) - 1) {
        prefix_len = sizeof(buf) - 2;                     // 留 1 字节给换行 + 1 给 '\0'
    }

    va_list ap;
    va_start(ap, fmt);
    int body_ret = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    va_end(ap);

    if (body_ret < 0) return;   // 格式化失败,丢弃

    size_t total = prefix_len + static_cast<size_t>(body_ret);   // 同样可能是「本应写入」
    if (total >= sizeof(buf)) total = sizeof(buf) - 1;
    buf[total++] = '\n';

    // ③ 异步落盘:走 AsyncLogger 单例(buffer 双缓冲 + 后台 flush 线程)
    AsyncLogger::instance().append(buf, total);
}

} // namespace gateway