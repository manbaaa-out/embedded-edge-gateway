#pragma once

// 网关应用对象:持有各层资源、装配事件源、运行主循环,不含业务逻辑本身。
// 业务分别位于 NodeLink(串口收发与组帧)、CommandTracker(seq 分配与超时重发)、
// decodeTelemetry(帧 → 记录)、translateCommand(topic → 命令)、
// TelemetryPipeline(遥测落库)。
//
// ---- 进程的线程构成 ----
// 恰好五条,每条都有明确且互不重叠的职责(用 /proc/<pid>/task 数过):
//   主 Reactor 线程    epoll 派发:串口收帧、下行发命令、超时重发、信号处理。
//                      所有共享状态(在途表、串口 fd、epoll 注册表)都只被它碰。
//   遥测写线程         TelemetryPipeline 内部,独占 SQLite 写连接,批量落库。
//   mosquitto 网络线程 libmosquitto 的 loop_start 起的,负责真正的 socket 收发。
//   HTTP 监控线程      独占只读连接,与写连接经 WAL 实现读写并发。
//   日志 flush 线程    AsyncLogger 单例在第一次 LOG_* 时起,全进程唯一的日志 fd 写者。
//                      它不归本类创建,但确实在进程里 —— 早先这里写「四条」漏的就是它。
// 线程之间一律用队列通信,不共享可变状态,因此进程里只剩队列自身那几把锁。
//
// 成员声明顺序即析构逆序,不可重排:pipeline_ 必须声明在 client_ 之后,
// 才能先于它析构 —— 写线程要先 join 掉,后面的资源才可以撤。

#include "gateway/cloud/CommandTranslator.h"
#include "gateway/cloud/MqttClient.h"
#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include "gateway/io/event/EventLoop.h"
#include "gateway/link/CommandTracker.h"
#include "gateway/link/NodeLink.h"
#include "gateway/pipeline/TelemetryPipeline.h"
#include "gateway/storage/Database.h"

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

namespace gateway {

class GatewayApp {
public:
    // 屏蔽 SIGHUP / SIGTERM / SIGINT 的默认递送,改由 signalfd 在主循环里取。
    //
    // 前置条件:必须早于任何线程的创建。信号掩码按线程生效,新线程继承创建者的掩码;
    // 若晚于线程创建,HTTP 线程、遥测写线程与 mosquitto 网络线程都不屏蔽这些信号,
    // 内核会在未屏蔽的线程中任选一个递送,并执行默认动作 —— 三个信号的默认动作都是
    // 终止进程,于是 SIGHUP 变成"reload 一下网关就死了",SIGTERM 变成"停机时进程被
    // 就地打死,异步日志缓冲里最后几秒的日志随之消失"。
    //
    // 返回 false 表示屏蔽失败,调用方可据此判定热加载与优雅停机不可用(非致命:
    // 此时行为退回默认动作,与加这套机制之前一致)。
    static bool blockManagedSignals();


    // 读取 ConfigManager::current() 的启动快照,打开 db / MQTT / 只读连接、
    // HTTP 线程、遥测写线程与串口。任一失败抛异常,由 main 致命退出交 systemd 重启。
    // ConfigManager::init 由 main 负责:配置读不出是更早的致命错误。
    GatewayApp();

    // 停掉 HTTP 线程并 join 它,之后才轮到各成员按声明逆序析构。
    //
    // 必须 join 而不是 detach:main 返回后会跑静态对象的析构,AsyncLogger 单例
    // 就在其中。若此时 HTTP 线程还活着并调 LOG_*,那就是往已析构的对象上写。
    // 换句话说,「优雅停机」和「后台线程 detach」不能共存,必须二选一。
    ~GatewayApp();

    GatewayApp(const GatewayApp&)            = delete;
    GatewayApp& operator=(const GatewayApp&) = delete;

    // 装配四类事件源并运行主循环。收到 SIGTERM / SIGINT 后主循环退出,
    // 函数返回,进程随即走完析构。返回进程退出码。
    int run();

private:
    // ---- 事件源装配 ----
    void setupSerial();      // 串口 Channel(ET)
    bool setupDownlink();    // eventfd + 下行 Channel(失败致命)
    void startMqtt();        // 挂下行 handler + 订阅 + loopStart
    void setupCmdTimer();    // 超时重试 timerfd(失败降级)
    void setupSignal();      // SIGHUP / SIGTERM / SIGINT signalfd(失败降级)

    // ---- 各事件源的处理 ----
    void onFrame(const Frame& f);        // 串口来帧:按 TYPE 分流
    void onAckFrame(const Frame& f);     // 应答帧:配对销账 + 回 MQTT
    void onDownlinkWakeup();             // eventfd:取空队列、组帧、发送、登记在途
    void onCmdTimer();                   // timerfd:推进在途表,重发 / 判死
    void reloadConfig();                 // SIGHUP:load-then-swap 后按 diff 重建

    std::shared_ptr<channel> makeSerialChannel();

    // 成员顺序即析构逆序,不可重排(见文件头)
    std::shared_ptr<MqttClient> client_;        // 上行 / 应答 MQTT
    std::shared_ptr<Database>   roDb_;          // HTTP 只读连接(WAL 读写并发)
    std::atomic<bool>           http_stop_{false};  // 停机意愿,HTTP 线程每秒轮询一次
    std::thread                 http_thread_;   // HTTP 监控线程(join,持 roDb_ 副本)

    std::unique_ptr<NodeLink>          link_;      // 与 STM32 的串口链路
    // 遥测落库(内含写线程,独占 SQLite 写连接)。app 层不持有写连接:
    // 它整个交给了 pipeline_,热加载换库也是投指令而非改指针。
    std::optional<TelemetryPipeline>   pipeline_;
    CommandTracker                     tracker_;   // 在途命令表(仅主线程访问,无锁)
    ThreadSafeQueue<DownCmd>           cmd_queue_; // mosquitto 线程 push / 主线程 try_pop

    EventLoop                loop_;            // 主 Reactor
    std::shared_ptr<channel> serial_channel_;  // 串口 Channel(热加载会换)

    int evfd_        = -1;   // 下行唤醒(close 由 channel RAII 负责)
    int cmd_timerfd_ = -1;   // 超时扫描
    int sfd_         = -1;   // SIGHUP signalfd
};

}  // namespace gateway
