#pragma once

#include "gateway/core/log/Logger.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <pthread.h>

namespace gateway {

// 一个有界、顺序执行的邮箱。reserve 仅供已接收任务的完成事件使用；生产者不等空位。
// 关闭后拒绝新任务，已接收任务按 FIFO 排空。stop() 由拥有者线程调用，不能在任务内调用。
class TaskExecutor {
public:
    explicit TaskExecutor(std::string name, std::size_t capacity = 1024,
                          std::size_t reserve = 0)
        : capacity_(capacity), reserve_(reserve), worker_([this, name = std::move(name)] {
            pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
            run();
        }) {}
    ~TaskExecutor() { stop(); }
    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    bool post(std::function<void()> task, bool completion = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto limit = capacity_ + (completion ? reserve_ : 0);
        if (closed_ || tasks_.size() >= limit) return false;
        tasks_.push_back(std::move(task));
        ready_.notify_one();
        return true;
    }

    // 不启动/创建额外线程；join 等待当前消费者排空，调用方须先停止外部生产者。
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return closed_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { task(); }
            catch (const std::exception& error) { LOG_ERROR("executor task failed: %s", error.what()); }
            catch (...) { LOG_ERROR("%s", "executor task failed with unknown exception"); }
        }
    }

    std::size_t capacity_;
    std::size_t reserve_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;
    std::thread worker_;
};

} // namespace gateway
