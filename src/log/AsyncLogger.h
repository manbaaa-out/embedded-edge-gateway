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
    explicit AsyncLogger(const std::string& filepath, int flush_intercal_sec = 3);

    ~AsyncLogger();

    static AsyncLogger& instance();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void append(const char* msg, size_t len);

private:
    using Buffer = std::string;
    using BufferPtr = std::unique_ptr<Buffer>;
    static constexpr size_t kBufferSize = 4 * 1024 * 1024;

    std::mutex mtx_;
    std::condition_variable cv_;
    BufferPtr currentBuffer_;
    BufferPtr nextBuffer_;
    std::vector<BufferPtr> bufferToWrite_;

    std::thread thread_;
    std::string filepath_;
    int flush_interval_sec_;
    std::atomic<bool> running_;

    void flushThread();

};
}
