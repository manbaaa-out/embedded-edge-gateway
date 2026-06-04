#pragma once
#include "ThreadSafeQueue.h"
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
