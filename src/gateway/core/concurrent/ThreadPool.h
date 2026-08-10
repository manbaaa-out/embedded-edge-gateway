#pragma once

// 通用任务线程池。**当前网关未使用它**,保留为 core 层的一个并发原语。
//
// 曾用于遥测双写(每条记录一个 task),后来移除。原因不是「线程开多了」,
// 而是两条结构性的:
//   1. 落库是串行的 —— N 个 worker 全撞在 SQLite 连接的那把锁上,并行度被锁
//      压回 1,只多付了锁竞争与上下文切换。真正的修法是消除共享而不是保护共享,
//      于是改成了单写线程 + 队列,见 TelemetryPipeline.h。
//   2. submit 收的是 std::function<void()>,一个不透明闭包。池看不见闭包里在做
//      什么,结构上就不可能把 N 条记录合并进一个事务 —— 而事务批处理正是那条
//      路径上最大的收益。要批处理,队列就必须是有类型的。
//
// 那什么时候该用它?按 λ×W 判断:要喂饱一条 worker,需要「到达率 × 单项耗时」
// 不小于 1。当前遥测负载约 1.5 项/秒,单项几十微秒,乘积在 1e-3 量级 —— 差了
// 三个数量级。要让它成立,得有一批彼此独立、单项数百毫秒以上、且不争同一个
// 串行资源的工作,例如:按设备并行的模型训练/基线重建、断网恢复后的历史数据
// 批量补传。那类工作出现时,这个池才有正当用途。
//
// 保留而不删除,是因为上述场景已在规划中;它不参与任何链接产物,留着零成本。

#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include <vector>
#include <thread>
#include <functional>
#include <future>

namespace gateway {

class ThreadPool {
    private:
    ThreadSafeQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers;

    public:
    ThreadPool(size_t n) {
        for (size_t i = 0; i < n; i++) {
            workers.emplace_back([this](){workerLoop();});
        }
    }

    template <typename F>
    auto submit(F func) -> std::future<decltype(func())> {
        using R = decltype(func());

        auto task_ptr = std::make_shared<std::packaged_task<R()>>(std::move(func));

        queue_.push([task_ptr](){(*task_ptr)();});

        std::future<R> fut = task_ptr->get_future();

        return fut;
    }

    ~ThreadPool() {
        queue_.shutdown();
        for (size_t i = 0; i < workers.size(); i++) {
            if (workers[i].joinable()) workers[i].join();
        }
    }
    private:
    void workerLoop() {
        while (true) {
            std::optional<std::function<void()>> task = queue_.pop();
            if (!task) break;
            (*task)();
        }
    }

};

}
