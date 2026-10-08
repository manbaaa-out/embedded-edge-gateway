#pragma once

#include "gateway/app/GatewayCore.h"
#include "gateway/app/ManagementReactor.h"
#include "gateway/cloud/MqttClient.h"
#include "gateway/core/concurrent/TaskExecutor.h"
#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include "gateway/core/config/Config.h"
#include "gateway/io/event/EventLoop.h"
#include "gateway/link/NodeLink.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace gateway {

// 组合根：串口、MQTT、HTTP 和管理信号分别由独立 I/O 循环接收。
// GatewayCore 处理应用业务；管理工作线程顺序执行资源准备与 MQTT 发布/替换。
class GatewayApp {
public:
    GatewayApp();
    ~GatewayApp();
    GatewayApp(const GatewayApp&) = delete;
    GatewayApp& operator=(const GatewayApp&) = delete;
    int run();

private:
    struct Downlink { uint64_t id; DownCmd command; };
    void registerSerialEvent();
    bool registerDownlinkEvent();
    void registerCommandTimerEvent();
    void onSerialEvent();
    void onSerialWriteEvent();
    void onDownlinkEvent();
    void onCommandTimerEvent();
    void dispatchFrame(const Frame& frame);
    void applyTrackerActions(const TrackerActions& actions);
    void queueTransmission(const CommandSend& send);
    void updateSerialWriteInterest();
    void notifyDownlink();
    bool submitCommand(uint64_t id, DownCmd command);
    bool publish(std::string topic, std::string payload, int qos, std::function<void()> done);
    void requestStop(); // 管理线程只设置原子状态并唤醒主线程，不跨线程操作串口 loop。
    void requestReload();
    void reloadConfig(); // management_worker_ 线程；仅资源交付动作回到串口 Reactor。
    std::unique_ptr<MqttClient> createMqttClient(const Config& config);
    void startHttpMonitor(int port);
    bool reloadDatabase(const Config& config);
    bool reloadMqtt(const Config& config);
    bool reloadSerial(const Config& config);

    // read_db_ 必须比 pipeline_ 活得更久，切库完成回调会原子发布读连接。
    std::shared_ptr<Database> read_db_;
    std::optional<TelemetryPipeline> pipeline_;
    std::shared_ptr<NodeLink> link_;
    CommandTracker tracker_;
    std::map<uint8_t, uint64_t> requests_; // 当前 SR 会话 seq -> 业务请求 ID，仅 Reactor。
    ThreadSafeQueue<Downlink> cmd_queue_{GatewayCore::kMaxCommands};
    ThreadSafeQueue<std::shared_ptr<NodeLink>> replacements_{8};
    EventLoop loop_;
    std::shared_ptr<channel> serial_channel_;
    int downlink_fd_ = -1;
    int command_timer_fd_ = -1;
    // 替换和 publish 仅在 management_worker_ 访问；启动前初始化、management_worker_ join 后销毁。
    std::unique_ptr<MqttClient> mqtt_client_;
    std::unique_ptr<GatewayCore> core_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> reload_pending_{false};
    std::atomic<bool> http_stop_{false};
    std::atomic<bool> http_failed_{false};
    std::atomic<bool> management_failed_{false};
    std::thread http_thread_;
    TaskExecutor management_worker_{"gateway-manage", 1024, GatewayCore::kMaxCommands + 16};
    // 最后创建、最先停止；其回调只向上述仍存活的状态/执行器交付事件。
    std::unique_ptr<ManagementReactor> management_reactor_;
};

} // namespace gateway
