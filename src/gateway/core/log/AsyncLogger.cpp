#include "gateway/core/log/AsyncLogger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace gateway {

    static ssize_t safe_write(int fd, const void* buf, size_t n) {
        size_t total = 0;                       // 与写入长度同为无符号,免去来回强转
        const char* p = static_cast<const char*>(buf);
        while(total < n) {
            ssize_t n_write = write(fd, p + total, n - total);
            if (n_write < 0) {
                if( errno == EINTR) continue;
                return -1;
            }
            total += static_cast<size_t>(n_write);   // n_write >= 0
        }

        return static_cast<ssize_t>(total);
    }

    // 进程单例写 stderr,不写文件。两个理由:
    // 一是全进程只剩一个日志出口 —— 直接 fprintf(stderr) 的那几处已改走 LOG_*,
    //   同一次故障的线索不会再分散在文件与 journal 两个地方;
    // 二是原先的 /tmp/gateway.log 在多数发行版上是 tmpfs,重启即失,
    //   而"排查一次开机后的故障"恰恰是最需要日志的场景。
    // 交给 journald 之后,轮转、持久化(Storage=persistent)、按时间/单元过滤
    // 都是它的既有能力,进程侧不必自己实现。
    AsyncLogger& AsyncLogger::instance() {
        static AsyncLogger inst("", 3);
        return inst;
    }

    AsyncLogger::AsyncLogger(const std::string& filepath, int flush_interval_sec)
    : currentBuffer_(std::make_unique<Buffer>()),nextBuffer_(std::make_unique<Buffer>()),filepath_(filepath),
    flush_interval_sec_(flush_interval_sec),running_(true) {
        currentBuffer_->reserve(kBufferSize);
        nextBuffer_->reserve(kBufferSize);

        bufferToWrite_.reserve(16);

        thread_ = std::thread([this](){flushThread();});

    }

    AsyncLogger::~AsyncLogger() {
        running_.store(false);
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // 本文件内的报错一律直接 fprintf(stderr),不走 LOG_*:
    // LOG_* 的终点就是本类,日志系统自身的故障若再交给它上报,轻则递归,
    // 重则在持锁路径上自我阻塞。这是唯一容许绕开统一日志通道的地方。
    void AsyncLogger::append(const char* msg, size_t len) {
        if (len > kBufferSize) {
            fprintf(stderr, "AsyncLogger: 日志信息将被截断\n");
            len = kBufferSize;
        }

        std::lock_guard<std::mutex> lock(mtx_);

        if (len + currentBuffer_->size() < kBufferSize) {
            currentBuffer_->append(msg, len);
            return;
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
        // 空路径 = 写 stderr。此时 fd 是借来的,不能 close:关掉 fd 2 之后
        // 后面所有诊断输出(含本函数自己的报错)都会静默消失。
        const bool to_stderr = filepath_.empty();

        int fd = to_stderr ? STDERR_FILENO
                           : open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "AsyncLogger: open '%s' failed: %s\n",
                   filepath_.c_str(), strerror(errno));

            return;
        }

        // 预分配备用 buffer,用于在交换出当前 buffer 后立即补位
        BufferPtr newCurrent = std::make_unique<Buffer>();
        BufferPtr newNext    = std::make_unique<Buffer>();
        newCurrent->reserve(kBufferSize);
        newNext->reserve(kBufferSize);

        std::vector<BufferPtr> localBuffers;
        localBuffers.reserve(16);

        while(running_.load()) {
            std::unique_lock<std::mutex> lock(mtx_);
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

            localBuffers.swap(bufferToWrite_);
            lock.unlock();

            for (const auto& buf:localBuffers) {
                ssize_t r = safe_write(fd, buf->data(), buf->size());
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

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (currentBuffer_ && !currentBuffer_->empty()) {
                bufferToWrite_.push_back(std::move(currentBuffer_));
            }
            localBuffers.swap(bufferToWrite_);
        }

        for (const auto& buf: localBuffers) {
            ssize_t r = safe_write(fd, buf->data(), buf->size());
            if (r < 0) {
                fprintf(stderr, "AsyncLogger: write failed: %s\n", strerror(errno));
            }

        }

        if (!to_stderr) close(fd);

    }
}
