#pragma once

#include "gateway/io/event/EventLoop.h"

#include <atomic>
#include <functional>
#include <thread>

namespace gateway {

// 独立管理事件入口。回调只投递拥有数据的事件；资源重建和 join 由外部拥有者执行。
class ManagementReactor {
public:
    using Callback = std::function<void()>;

    // 必须在创建任何后台线程前调用，使所有线程继承托管信号屏蔽集。失败抛异常。
    static void blockManagedSignals();

    // 当前线程须已屏蔽 SIGHUP/SIGTERM/SIGINT。所有 fd 同步创建并注册成功后才启动线程。
    // reload/shutdown/failed 在 gateway-admin 线程调用，不得阻塞、join 或销毁本组件。
    ManagementReactor(Callback reload, Callback shutdown, Callback failed);
    ~ManagementReactor();
    ManagementReactor(const ManagementReactor&) = delete;
    ManagementReactor& operator=(const ManagementReactor&) = delete;

    // 由拥有者线程调用，允许顺序重复调用。唤醒空闲 epoll 并 join，不产生 shutdown 事件。
    void stop();

private:
    void onSignals();
    void onStop();
    void run() noexcept;

    Callback reload_;
    Callback shutdown_;
    Callback failed_;
    // loop_ 独占两个 channel/fd；构造异常自动回收，析构前必须先 join thread_。
    EventLoop loop_;
    int signal_fd_ = -1;
    int stop_fd_ = -1;
    std::atomic<bool> stopping_{false};
    bool shutdown_sent_ = false; // 仅管理 Reactor 线程读写。
    std::thread thread_;
};

} // namespace gateway
