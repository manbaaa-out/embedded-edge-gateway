#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace gateway {

class AsyncLogger {

public:
    // filepath 为空表示写 stderr(fd 2),这也是进程单例走的路径:
    // 交由 systemd/journald 负责轮转与持久化,进程自己不管文件生命周期。
    // 传入非空路径则打开该文件追加写,供单测与非 systemd 部署使用。
    explicit AsyncLogger(const std::string& filepath, int flush_intercal_sec = 3);

    ~AsyncLogger();

    static AsyncLogger& instance();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void append(const char* msg, size_t len);

private:
    using Buffer = std::string;
    using BufferPtr = std::unique_ptr<Buffer>;

    // 单条日志上限,与 Logger::log 的栈缓冲同宽:那里格式化完的整行(含结尾
    // 换行)最长就是 1024 字节,超出者在进 append 之前已被截断。此处独立成常量,
    // 是为了不再和 kBufferSize 绑在一起 —— 二者约束的是完全不同的东西。
    static constexpr size_t kMaxLogLine = 1024;

    // 单块缓冲大小。取 64KB 而非更大,有两层考虑:
    // 一是它决定"攒够多少字节才换页并唤醒后台线程"。稳态下换页几乎不发生,
    //   落盘全靠 flush_interval_sec_ 的超时;但日志风暴时它就是唯一的及时触发者,
    //   块越大,进程被 SIGKILL 或掉电打死时压在内存里没写出去的就越多,
    //   而那恰好是最需要日志的时刻。
    // 二是停在 glibc 默认 mmap 阈值(128KB)以下。flushThread 每轮只回收两块,
    //   其余在 localBuffers 析构时归还、下轮重新分配;留在阈值内这些分配走堆,
    //   越过去就变成一对 mmap/munmap 系统调用。
    static constexpr size_t kBufferSize = 64 * 1024;

    // 待写队列的硬上限,乘上 kBufferSize 就是日志占用内存的天花板(4MB)。
    // 没有它时 bufferToWrite_ 可以无限涨:后端一旦沉不下去(journald 卡住、
    // 磁盘满、写 fd 阻塞),前端仍在 append,最后由 OOM killer 收场。
    static constexpr size_t kMaxQueuedBuffers = 64;

    // 溢出时保留最早的几块。一次故障风暴的根因几乎总在最前面那几行,
    // 尾部多是同一原因的重复刷屏,所以丢尾不丢头。
    static constexpr size_t kKeepOnOverflow = 2;

    std::mutex mtx_;
    std::condition_variable cv_;
    BufferPtr currentBuffer_;
    BufferPtr nextBuffer_;
    std::vector<BufferPtr> bufferToWrite_;

    // 溢出丢弃的缓冲块数,mtx_ 保护。由 append 累加,flushThread 取走并清零后
    // 补记一行到日志流里 —— 丢了多少必须留下痕迹,否则读日志的人只会看到
    // 一段莫名其妙的空白。
    size_t dropped_ = 0;

    std::thread thread_;
    std::string filepath_;
    int flush_interval_sec_;
    std::atomic<bool> running_;

    void flushThread();

};
}
