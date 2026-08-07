#pragma once

// 网关应用对象:持有各层资源、装配事件源、运行主循环,不含业务逻辑本身。
// 业务分别位于 NodeLink(串口收发与组帧)、CommandTracker(seq 分配与超时重发)、
// decodeTelemetry(帧 → 记录)、translateCommand(topic → 命令)、
// TelemetryPipeline(双写扇出)。
//
// 成员声明顺序即析构逆序,不可重排:pool_ 必须声明在 db_ / client_ 之后,
// 才能先于它们析构。线程池析构时要 drain 在飞的 task,而这些 task 经
// shared_ptr 快照持有 db / client;若 db_ 先析构,drain 中的 task 将访问已析构对象。

#include "gateway/cloud/CommandTranslator.h"
#include "gateway/cloud/MqttClient.h"
#include "gateway/core/concurrent/ThreadPool.h"
#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include "gateway/io/event/EventLoop.h"
#include "gateway/link/CommandTracker.h"
#include "gateway/link/NodeLink.h"
#include "gateway/pipeline/TelemetryPipeline.h"
#include "gateway/storage/Database.h"

#include <memory>
#include <optional>
#include <thread>

namespace gateway {

class GatewayApp {
public:
    // 屏蔽 SIGHUP 的默认递送。
    //
    // 前置条件:必须早于任何线程的创建。信号掩码按线程生效,新线程继承创建者的掩码;
    // 若晚于线程创建,HTTP 线程、线程池 worker 与 mosquitto 网络线程都不屏蔽 SIGHUP,
    // 内核会在未屏蔽的线程中任选一个递送,并执行默认动作终止进程。
    //
    // 返回 false 表示屏蔽失败,调用方可据此判定热加载不可用(非致命)。
    static bool blockReloadSignal();


    // 读取 ConfigManager::current() 的启动快照,打开 db / MQTT / 只读连接、
    // HTTP 线程、线程池与串口。任一失败抛异常,由 main 致命退出交 systemd 重启。
    // ConfigManager::init 由 main 负责:配置读不出是更早的致命错误。
    GatewayApp();

    GatewayApp(const GatewayApp&)            = delete;
    GatewayApp& operator=(const GatewayApp&) = delete;

    // 装配四类事件源并运行主循环(永久阻塞)。返回进程退出码。
    int run();

private:
    // ---- 事件源装配 ----
    void setupSerial();      // 串口 Channel(ET)
    bool setupDownlink();    // eventfd + 下行 Channel(失败致命)
    void startMqtt();        // 挂下行 handler + 订阅 + loopStart
    void setupCmdTimer();    // 超时重试 timerfd(失败降级)
    void setupSignal();      // SIGHUP signalfd(失败降级)

    // ---- 各事件源的处理 ----
    void onFrame(const Frame& f);        // 串口来帧:按 TYPE 分流
    void onAckFrame(const Frame& f);     // 应答帧:配对销账 + 回 MQTT
    void onDownlinkWakeup();             // eventfd:取空队列、组帧、发送、登记在途
    void onCmdTimer();                   // timerfd:推进在途表,重发 / 判死
    void reloadConfig();                 // SIGHUP:load-then-swap 后按 diff 重建

    std::shared_ptr<channel> makeSerialChannel();

    // 成员顺序即析构逆序,不可重排(见文件头)
    std::shared_ptr<Database>   db_;            // 上行写连接(线程池 task 持快照)
    std::shared_ptr<MqttClient> client_;        // 上行 / 应答 MQTT
    std::shared_ptr<Database>   roDb_;          // HTTP 只读连接(WAL 读写并发)
    std::thread                 http_thread_;   // HTTP 监控线程(detach,持 roDb_ 副本)
    std::unique_ptr<ThreadPool> pool_;          // 双写线程池(最先析构,drain 在飞 task)

    std::unique_ptr<NodeLink>          link_;      // 与 STM32 的串口链路
    std::optional<TelemetryPipeline>   pipeline_;  // 双写扇出(需在 pool_ 之后构造)
    CommandTracker                     tracker_;   // 在途命令表(仅主线程访问,无锁)
    ThreadSafeQueue<DownCmd>           cmd_queue_; // mosquitto 线程 push / 主线程 try_pop

    EventLoop                loop_;            // 主 Reactor
    std::shared_ptr<channel> serial_channel_;  // 串口 Channel(热加载会换)

    int evfd_        = -1;   // 下行唤醒(close 由 channel RAII 负责)
    int cmd_timerfd_ = -1;   // 超时扫描
    int sfd_         = -1;   // SIGHUP signalfd
};

}  // namespace gateway
