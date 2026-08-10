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

    bool push(T value) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_cv_.wait(lock, [this](){return capacity_ == 0 || queue_.size() < capacity_ || shutdown_;});

        if (shutdown_) return false;
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_cv_.notify_one();
        return true;
    }

    // 队列满则立刻返回 false 而不等待,供「宁可丢也不能阻塞生产者」的场景使用。
    //
    // 与 push 的取舍按数据性质决定,不是风格差异:
    //   下行命令  用 push     —— 命令不可丢,阻塞投递方是可接受的代价;
    //   上行遥测  用 try_push —— 生产者是主 Reactor 线程,一旦阻塞,串口收帧、
    //                           ACK 配对、超时重发、SIGTERM 会一起停摆。
    //                           传感器读数丢一条无关紧要,Reactor 停住是事故。
    bool try_push(T value) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (shutdown_) return false;
        if (capacity_ != 0 && queue_.size() >= capacity_) return false;
        queue_.push(std::move(value));
        not_empty_cv_.notify_one();
        return true;
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

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop();
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

