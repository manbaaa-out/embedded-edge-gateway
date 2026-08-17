// 异步日志缓冲区交换与输出实现。
//
// 前端线程只在 mtx_ 临界区内修改 currentBuffer_、nextBuffer_ 和待写队列；后台线程
// 把所有权交换到 localBuffers 后立即解锁，再执行可能阻塞的 write。输出 fd 只有后台
// 线程访问，析构通过 running_ 和 join 建立最后一次排空与对象销毁之间的顺序。

#include "gateway/core/log/AsyncLogger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace gateway {

    /**
     * @brief 重试 EINTR 和短写，直到写满或发生不可恢复错误。
     * @param fd 借用的输出描述符，本函数不关闭。
     * @param buf 指向至少 n 个可读字节。
     * @param n 请求写入的字节数。
     * @return 成功时为 n，发生不可恢复 write 错误时为 -1。
     * @pre n 不大于 ssize_t 的可表示上限。
     * @pre 对该 fd 的非零长度 write 要么取得正进展，要么返回错误；返回 0 时本循环
     *      当前不会退出。
     */
    static ssize_t safe_write(int fd, const void* buf, size_t n) {
        size_t total = 0; // 已成功交给内核的前缀字节数。
        const char* p = static_cast<const char*>(buf); // 用于字节偏移的只读输入视图。
        while(total < n) {
            ssize_t n_write = write(fd, p + total, n - total); // 本轮系统调用写入量。
            if (n_write < 0) {
                if( errno == EINTR) continue;
                return -1;
            }
            total += static_cast<size_t>(n_write);
        }

        return static_cast<ssize_t>(total);
    }

    /**
     * @param fd 当前日志输出描述符，借用且不关闭。
     * @param dropped 待报告的丢弃缓冲块数量；0 时无操作。
     */
    static void report_dropped(int fd, size_t dropped) {
        if (dropped == 0) return;

        char note[128]; // 固定大小的内部诊断行，不经过 AsyncLogger::append()。
        int n = snprintf(note, sizeof(note),
                         "[WARN ] AsyncLogger 待写队列溢出,丢弃 %zu 块缓冲\n", dropped);
        // n 是完整诊断所需字符数；负值表示格式化失败。
        if (n <= 0) return;

        size_t len = static_cast<size_t>(n); // note 中实际提交给 safe_write 的字节数。
        if (len >= sizeof(note)) len = sizeof(note) - 1;
        safe_write(fd, note, len);
    }

    // 进程单例写 stderr，由服务管理器负责收集和持久化。
    AsyncLogger& AsyncLogger::instance() {
        static AsyncLogger inst("", 3); // 首次使用时构造，进程静态析构阶段停止。
        return inst;
    }

    AsyncLogger::AsyncLogger(const std::string& filepath, int flush_interval_sec)
    : currentBuffer_(std::make_unique<Buffer>()),nextBuffer_(std::make_unique<Buffer>()),filepath_(filepath),
    flush_interval_sec_(flush_interval_sec),running_(true) {
        currentBuffer_->reserve(kBufferSize);
        nextBuffer_->reserve(kBufferSize);

        // 避免高负载下在锁内扩容待写队列。
        bufferToWrite_.reserve(kMaxQueuedBuffers);

        thread_ = std::thread([this](){flushThread();});

    }

    AsyncLogger::~AsyncLogger() {
        running_.store(false);
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // 后端自身的诊断必须绕过 LOG_*，否则会递归回到本对象。
    void AsyncLogger::append(const char* msg, size_t len) {
        if (len > kMaxLogLine) {
            fprintf(stderr, "AsyncLogger: 日志信息将被截断\n");
            len = kMaxLogLine;
        }

        std::lock_guard<std::mutex> lock(mtx_); // 保护容量判断、裁剪和缓冲区交换。

        if (len + currentBuffer_->size() < kBufferSize) {
            currentBuffer_->append(msg, len);
            return;
        }

        // 后端落后时丢弃队尾缓冲区并延后报告，锁内不执行任何输出操作。
        if (bufferToWrite_.size() >= kMaxQueuedBuffers) {
            dropped_ += bufferToWrite_.size() - kKeepOnOverflow;

            // 优先回收一个待丢弃的块作为前端备用缓冲区。
            if (!nextBuffer_) {
                nextBuffer_ = std::move(bufferToWrite_.back());
                nextBuffer_->clear();
            }

            bufferToWrite_.erase(
                bufferToWrite_.begin() + static_cast<std::ptrdiff_t>(kKeepOnOverflow),
                bufferToWrite_.end());
        }

        bufferToWrite_.push_back(std::move(currentBuffer_));

        if (nextBuffer_) {
            currentBuffer_ = std::move(nextBuffer_);
        }
        else {
            currentBuffer_ = std::make_unique<Buffer>();
            currentBuffer_->reserve(kBufferSize);
        }

        currentBuffer_->append(msg,len);
        cv_.notify_one();
    }

    void AsyncLogger::flushThread() {
        // stderr 的 fd 由进程环境持有，本类只关闭自行打开的文件。
        const bool to_stderr = filepath_.empty(); // 区分借用 fd 与本类拥有的文件 fd。

        int fd = to_stderr ? STDERR_FILENO
                           : open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        // fd 只在本线程使用；非 stderr 路径由本函数在退出前关闭。
        if (fd < 0) {
            fprintf(stderr, "AsyncLogger: open '%s' failed: %s\n",
                   filepath_.c_str(), strerror(errno));

            return;
        }

        // 预分配两个缓冲区，使常规交换路径无需分配内存。
        BufferPtr newCurrent = std::make_unique<Buffer>(); // 交换给前端的首个空块。
        BufferPtr newNext    = std::make_unique<Buffer>(); // 交换给前端的备用空块。
        newCurrent->reserve(kBufferSize);
        newNext->reserve(kBufferSize);

        std::vector<BufferPtr> localBuffers; // 后台线程在锁外独占的本轮待写块。
        localBuffers.reserve(kMaxQueuedBuffers);

        while(running_.load()) {
            std::unique_lock<std::mutex> lock(mtx_); // wait_for 和指针交换所需的锁。
            cv_.wait_for(lock, std::chrono::seconds(flush_interval_sec_), [this](){
                return !bufferToWrite_.empty() || !running_.load();
            });

            if (currentBuffer_ && !currentBuffer_->empty()) {
                bufferToWrite_.push_back(std::move(currentBuffer_));
                currentBuffer_ = std::move(newCurrent);
            }

            if (!nextBuffer_) {
                nextBuffer_ = std::move(newNext);
            }

            if (bufferToWrite_.empty()) {
                continue;
            }

            // 与待写缓冲区一起取走丢弃计数，确保报告不会先于对应缺口。
            const size_t dropped = dropped_; // 与本批日志对应的溢出块数快照。
            dropped_ = 0;

            localBuffers.swap(bufferToWrite_);
            lock.unlock();

            report_dropped(fd, dropped);

            for (const auto& buf:localBuffers) { // buf 是本轮按队列顺序写出的独占块。
                ssize_t r = safe_write(fd, buf->data(), buf->size()); // 本块写入结果。
                if (r < 0) {
                    fprintf(stderr, "AsyncLogger: write failed: %s\n", strerror(errno));
                }

            }

            if (!newCurrent) {
                newCurrent = std::move(localBuffers.back());
                localBuffers.pop_back();
                newCurrent->clear();
            }
            if (!newNext && !(localBuffers.empty()))
            {
                newNext = std::move(localBuffers.back());
                localBuffers.pop_back();
                newNext->clear();
            }

            localBuffers.clear();


        }

        size_t dropped_at_exit = 0; // 主循环停止后尚未报告的溢出块数。
        {
            std::lock_guard<std::mutex> lock(mtx_); // 原子取得最终前端缓冲和待写队列。
            if (currentBuffer_ && !currentBuffer_->empty()) {
                bufferToWrite_.push_back(std::move(currentBuffer_));
            }
            localBuffers.swap(bufferToWrite_);
            dropped_at_exit = dropped_;
            dropped_ = 0;
        }

        // 析构前排空剩余日志并补报最后一次溢出。
        report_dropped(fd, dropped_at_exit);

        for (const auto& buf: localBuffers) { // buf 是析构前必须写出的剩余块。
            ssize_t r = safe_write(fd, buf->data(), buf->size()); // 最终块写入结果。
            if (r < 0) {
                fprintf(stderr, "AsyncLogger: write failed: %s\n", strerror(errno));
            }

        }

        if (!to_stderr) close(fd);

    }
}
