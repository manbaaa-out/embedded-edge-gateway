#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace gateway{

template <typename T>
class ThreadSafeQueue {
public:

    ThreadSafeQueue(size_t capacity = 0): capacity_(capacity) {};

    ThreadSafeQueue(const ThreadSafeQueue& other) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue& other) = delete;

    void push(T value) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_cv_.wait(lock, [this](){return capacity_ == 0 || queue_.size() < capacity_ || shutdown_;});

        if (shutdown_) return;
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_cv_.notify_one();
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        not_empty_cv_.wait(lock, [this](){return !queue_.empty() || shutdown_;});

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_cv_.notify_one();
        return value;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx_);
        shutdown_ = true;
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable not_empty_cv_;
    std::condition_variable not_full_cv_;
    std::queue<T> queue_;
    bool shutdown_ = false;
    size_t capacity_ = 0;

};

}

