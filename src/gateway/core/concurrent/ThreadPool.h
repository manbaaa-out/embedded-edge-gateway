#pragma once

// 固定大小的通用任务池。
//
// 设计上，任务统一擦除为 void() 并进入 FIFO 队列；packaged_task 保存返回值或
// 异常，worker 只负责执行。至少有一个 worker 时，析构会关停入队、排空已提交
// 任务，再 join 全部线程。类本身不提供取消语义，submit() 不得与析构并发。

#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include <vector>
#include <thread>
#include <functional>
#include <future>

namespace gateway {

/** 拥有固定数量 worker 和一个共享任务队列的简易线程池。 */
class ThreadPool {
private:
    ThreadSafeQueue<std::function<void()>> queue_; /**< 等待 worker 执行的类型擦除任务。 */
    std::vector<std::thread> workers;              /**< 本对象拥有并在析构时 join 的线程。 */

public:
    /**
     * @brief 创建固定数量的工作线程。
     * @param n worker 数量；必须大于 0。传 0 时任务不会执行，对应
     *          future 只会在任务对象被销毁后以 broken_promise 结束。
     * @warning 当前构造过程未回收已启动的 worker；若后续线程创建或 vector
     *          扩容抛异常，对已启动 std::thread 的析构可导致 std::terminate。
     */
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++) { // i 仅用于准确创建 n 个同构 worker。
            workers.emplace_back([this](){workerLoop();});
        }
    }

    /**
     * @brief 将一个无参可调用对象加入任务队列。
     * @tparam F 可移动或可复制的无参可调用类型。
     * @param func 待执行任务；按值接收后移动进 packaged_task。
     * @return 与任务结果关联的 future；任务抛出的异常会由 future::get() 重抛。
     * @throws std::bad_alloc 任务封装或队列扩容失败。任务类型的移动异常也会原样传播。
     */
    template <typename F>
    auto submit(F func) -> std::future<decltype(func())> {
        using R = decltype(func()); // 调用 func() 得到的任务返回类型，可为 void。

        // shared_ptr 使复制进 std::function 的闭包可以共同拥有不可复制的 packaged_task。
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(std::move(func));

        queue_.push([task_ptr](){(*task_ptr)();});

        std::future<R> fut = task_ptr->get_future(); // 调用方观察结果和异常的唯一通道。

        return fut;
    }

    /** 永久关停队列，等待已提交任务执行完毕，并回收所有 worker。 */
    ~ThreadPool() {
        queue_.shutdown();
        for (size_t i = 0; i < workers.size(); i++) { // i 标识本轮等待的 worker。
            if (workers[i].joinable()) workers[i].join();
        }
    }
private:
    /** 单个 worker 的消费循环；nullopt 表示队列已关停且排空。 */
    void workerLoop() {
        while (true) {
            std::optional<std::function<void()>> task = queue_.pop(); // 当前取得的独占任务。
            if (!task) break;
            (*task)();
        }
    }

};

}
