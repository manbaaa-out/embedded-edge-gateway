#pragma once

// 支持有界或无界容量的多生产者、多消费者 FIFO 队列。
//
// 一把互斥量同时保护元素、容量和关停状态；两个条件变量分别表达“可读”和“可写”。
// shutdown() 是单向状态转换：之后所有入队操作失败，出队操作仍可排空已有元素，
// 最终以 nullopt 表达消费循环应退出。empty()/size() 仅提供瞬时观测，不参与同步决策。

#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace gateway{

/**
 * @tparam T 队列元素类型，须可移动构造。元素构造异常会向调用方传播；
 *            pop()/try_pop() 在 T 的移动构造抛出时不提供强异常保证。
 *
 * 析构函数不会自动通知等待者；调用方必须在对象销毁前结束并发访问，
 * 需要终止消费循环时应先调用 shutdown()。
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 创建空队列。
     * @param capacity 最大元素数；0 表示不设容量上限。
     */
    ThreadSafeQueue(size_t capacity = 0): capacity_(capacity) {};

    /** @param other 不会被读取；队列含独立同步状态，因此禁止复制构造。 */
    ThreadSafeQueue(const ThreadSafeQueue& other) = delete;
    /** @param other 不会被读取；禁止用其覆盖本队列的元素和关停状态。 */
    ThreadSafeQueue& operator=(const ThreadSafeQueue& other) = delete;

    /**
     * @brief 阻塞到元素入队或队列关停。
     * @param value 按值接收的元素，成功时移动进队列。
     * @return 成功入队返回 true；等待期间或调用前已关停则返回 false。
     */
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mtx_); // 条件等待期间可自动释放并重新取得互斥量。
        not_full_cv_.wait(lock, [this](){return capacity_ == 0 || queue_.size() < capacity_ || shutdown_;});

        if (shutdown_) return false;
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 在限定时间内尝试入队。
     * @tparam Rep timeout 的计数类型。
     * @tparam Period timeout 的时间单位比例。
     * @param value 按值接收的元素，成功时移动进队列。
     * @param timeout 队列满时允许等待的最长时长。
     * @return 成功入队返回 true；超时或关停返回 false。
     */
    template <typename Rep, typename Period>
    bool push_for(T value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mtx_); // wait_for 所需的可解锁互斥句柄。
        // ready 区分“条件满足”与“等待超时”；谓词同时响应容量释放和关停。
        const bool ready = not_full_cv_.wait_for(lock, timeout, [this](){
            return capacity_ == 0 || queue_.size() < capacity_ || shutdown_;
        });
        if (!ready || shutdown_) return false;
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 不等待地尝试入队。
     * @param value 按值接收的元素，成功时移动进队列。
     * @return 成功入队返回 true；容量已满或队列已关停返回 false。
     */
    bool try_push(T value) {
        std::lock_guard<std::mutex> lock(mtx_); // 覆盖状态检查与 push，避免超卖容量。
        if (shutdown_) return false;
        if (capacity_ != 0 && queue_.size() >= capacity_) return false;
        queue_.push(std::move(value));
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 阻塞到取得一个元素，或确认关停队列已排空。
     * @return 队首元素的移动结果；关停且排空后返回 nullopt。
     */
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mtx_); // 等待期间允许生产者取得互斥量。
        not_empty_cv_.wait(lock, [this](){return !queue_.empty() || shutdown_;});

        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front()); // 先取得元素所有权，再从容器删除节点。
        queue_.pop();
        lock.unlock();
        not_full_cv_.notify_one();
        return value;
    }

    /**
     * @brief 不等待地尝试取出队首元素。
     * @return 队首元素的移动结果；队列为空返回 nullopt，不区分是否已关停。
     */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14
    // GCC 13 会在部分非平凡 T 的移动构造上误报；仅在该实例化点抑制告警。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mtx_); // 覆盖判空、移动和 pop 的完整临界区。
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front()); // 返回给调用方的队首值。
        queue_.pop();
        not_full_cv_.notify_one();
        return value;
    }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14
#pragma GCC diagnostic pop
#endif

    /** @return 调用瞬间队列是否为空的快照；返回后可能立即过时。 */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_); // const 观测仍需与写操作同步。
        return queue_.empty();
    }

    /** @return 调用瞬间的元素数量快照；返回后可能立即过时。 */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_); // 保护底层容器的 size() 读取。
        return queue_.size();
    }

    /** 永久关停队列，并唤醒所有等待中的生产者和消费者。重复调用无额外效果。 */
    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx_); // 将状态切换与等待谓词原子化。
        shutdown_ = true;
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

private:
    mutable std::mutex mtx_;                 /**< 保护以下全部共享状态；mutable 支持 const 快照。 */
    std::condition_variable not_empty_cv_;  /**< 元素入队或关停时唤醒消费者。 */
    std::condition_variable not_full_cv_;   /**< 元素出队或关停时唤醒生产者。 */
    std::queue<T> queue_;                   /**< 按进入顺序保存尚未消费的元素。 */
    bool shutdown_ = false;                 /**< 一旦为 true 永不恢复为 false。 */
    size_t capacity_ = 0;                   /**< 0 为无界，否则是 queue_ 的元素上限。 */

};

}
