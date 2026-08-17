#pragma once

// 异步日志后端。
//
// 多个调用线程只在短临界区内把完整日志行复制到 currentBuffer_；后台线程批量交换
// 待写缓冲区，并在锁外独占输出 fd。待写队列设有裁剪阈值，后端长期阻塞时保留最早
// 的诊断块、丢弃其余积压并补记丢弃数量，以有界内存换取业务线程可继续运行。

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace gateway {

/** 线程安全的缓冲日志写入器；拥有一个后台线程以及可选的输出文件 fd。 */
class AsyncLogger {

public:
    /**
     * @brief 创建日志后端并立即启动 flush 线程。
     * @param filepath 空字符串表示借用 stderr；非空则以追加模式打开该文件。
     * @param flush_intercal_sec 未填满缓冲区时的最大刷新间隔，单位秒，必须大于 0。
     *
     * 文件由后台线程打开；打开失败会直接写 stderr，随后线程退出。
     */
    explicit AsyncLogger(const std::string& filepath, int flush_intercal_sec = 3);

    /**
     * 通知后台线程停止并等待退出。输出文件打开成功时，线程会在退出前
     * 尝试写完当前队列，再关闭本类拥有的 fd；打开或 write 失败无重试保证。
     * 调用方必须先保证不再有 append() 与析构并发。
     */
    ~AsyncLogger();

    /** @return 写 stderr、刷新间隔为 3 秒的进程级单例。 */
    static AsyncLogger& instance();

    /** @param other 不会被读取；线程、互斥量和缓冲区均不可复制。 */
    AsyncLogger(const AsyncLogger&) = delete;
    /** @param other 不会被读取；禁止覆盖本实例的后台线程和输出状态。 */
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /**
     * @brief 追加一条已格式化消息。
     * @param msg 非空输入指针，指向至少 len 个有效字节；内容不要求以 NUL 结尾。
     * @param len 消息字节数；大于 kMaxLogLine 时会截断并直接向 stderr 报告。
     *
     * 该函数可由多个业务线程并发调用。消息整体写入同一缓冲区，不会跨块拆分。
     * 换块时的内存分配失败会向调用方传播。
     */
    void append(const char* msg, size_t len);

private:
    using Buffer = std::string;             /**< 按字节保存多条完整日志行的连续缓冲区。 */
    using BufferPtr = std::unique_ptr<Buffer>; /**< 缓冲区的独占所有权，用于低成本交换。 */

    /** 单次 append 接受的最大字节数，与 Logger 的行缓冲区大小一致。 */
    static constexpr size_t kMaxLogLine = 1024;

    /** 聚合块容量上限；一行不会跨块，块满后立即唤醒后台线程。 */
    static constexpr size_t kBufferSize = 64 * 1024;

    /** 前端检测并裁剪积压的阈值；后台交换当前块时可能短暂多出一块。 */
    static constexpr size_t kMaxQueuedBuffers = 64;

    /** 触发溢出裁剪时保留的最早缓冲块数量。 */
    static constexpr size_t kKeepOnOverflow = 2;

    std::mutex mtx_;                    /**< 保护前端缓冲、待写队列和 dropped_。 */
    std::condition_variable cv_;        /**< 块就绪或析构时唤醒后台线程。 */
    BufferPtr currentBuffer_;           /**< 前端当前追加的缓冲块。 */
    BufferPtr nextBuffer_;              /**< 前端换块时优先复用的空闲块，可暂时为空。 */
    std::vector<BufferPtr> bufferToWrite_; /**< 等待后台线程取走的完整或待刷新块。 */

    size_t dropped_ = 0; /**< 尚未向输出报告的溢出丢弃块数，由 mtx_ 保护。 */

    std::thread thread_;           /**< 构造时启动、析构时 join 的唯一输出线程。 */
    std::string filepath_;         /**< 空表示 stderr，否则为后台线程打开的文件路径。 */
    int flush_interval_sec_;       /**< 定时刷新间隔，单位秒。 */
    std::atomic<bool> running_;    /**< true 时循环刷新；false 触发最后一次排空。 */

    /** 后台线程入口：交换待写块、执行 write，并在退出前排空剩余数据。 */
    void flushThread();

};
}
