#include "AsyncLogger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace gateway {

    static ssize_t safe_write(int fd, const void* buf, size_t n) {
        ssize_t total = 0;
        const char* p = static_cast<const char*>(buf);
        while(total < static_cast<ssize_t>(n)) {
            ssize_t n_write = write(fd, p + total, n - total);
            if (n_write < 0) {
                if( errno == EINTR) continue;
                return -1;
            }
            total += n_write;
        }

        return total;
    }

    AsyncLogger& AsyncLogger::instance() {
        static AsyncLogger inst("/tmp/gateway.log", 3);
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

    void AsyncLogger::append(const char* msg, size_t len) {
        if (len > kBufferSize) {
            fprintf(stderr, "日志信息将被截断");
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
        int fd = open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "AsyncLogger: open '%s' failed: %s\n",
                   filepath_.c_str(), strerror(errno));

            return;
        }

        // 备用 buffer(用于"挪走前先填上"的占位),从堆上预造
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

        close(fd);

    }
}
