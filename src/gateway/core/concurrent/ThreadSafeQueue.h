#pragma once

// 有界/无界阻塞队列,本项目全部跨线程移交的唯一通道。
//
// 这不只是「有个队列好用」:除队列自身这几把锁外,进程里没有别的锁 —— 在途命令表、
// 串口 fd、SQLite 写连接,每一样都只归一条线程独占,要传东西一律经由本队列。
// 这条纪律是整个并发设计的全部,别处不加锁不是忘了,是不需要。
//
// 两处用法刻意不同,见 try_push 与 try_pop 各自的说明。

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace gateway{

template <typename T>
class ThreadSafeQueue {
public:
    // capacity == 0 表示不限长
    ThreadSafeQueue(size_t capacity = 0): capacity_(capacity) {};

    ThreadSafeQueue(const ThreadSafeQueue& other) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue& other) = delete;

    // 阻塞入队:满则等。返回 false 表示队列已关停,调用方据此放弃这条数据。
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_cv_.wait(lock, [this](){return capacity_ == 0 || queue_.size() < capacity_ || shutdown_;});

        if (shutdown_) return false;
        queue_.push(std::move(value));
        lock.unlock();                   // 先解锁再通知,免得被唤醒者醒来又撞上锁
        not_empty_cv_.notify_one();
        return true;
    }

    // 非阻塞入队:满则立刻返回 false,供「宁可丢也不能阻塞生产者」的场景使用。
    //
    // 与 push 的取舍按数据性质定,不是风格差异:
    //   下行命令  用 push     —— 命令不可丢,而投递方是 mosquitto 线程,阻塞得起;
    //   上行遥测  用 try_push —— 投递方是主 Reactor,一旦阻塞,串口收帧、ACK 配对、
    //                           超时重发、SIGTERM 会一起停摆。丢一条读数无关紧要,
    //                           Reactor 停住是事故。
    bool try_push(T value) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (shutdown_) return false;
        if (capacity_ != 0 && queue_.size() >= capacity_) return false;
        queue_.push(std::move(value));
        not_empty_cv_.notify_one();
        return true;
    }

    // 阻塞取数,消费线程用。返回 nullopt 即「关停且已排空,可以退出了」——
    // 用类型表达退出信号,消费循环就能写成 while (auto job = q.pop())。
    //
    // 判据是 queue_.empty() 而不是 shutdown_:关停后仍先把剩余的取完。
    // 这一条直接决定停机是优雅 drain 还是粗暴丢弃。
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

    // 非阻塞取数,Reactor 线程用 —— 它绝不能阻塞在队列上。
    // 它靠 eventfd 得知「有货了」再来取:队列送数据,eventfd 送通知,两件事分开。
    // 而 eventfd 的计数会合并,所以调用方必须循环取空,不能只取一条。
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

    // 这两个的返回值天然是过时的 —— 拿到手时别的线程可能已经改了。
    // 只适合做日志与监控,不能用来做「先判空再 pop」这类决策。
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    // 单向,没有 restart —— 关停就是要退出了。
    // 这里用 notify_all 而非 notify_one:要叫醒的是所有人,一起退出。
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

