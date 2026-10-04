#pragma once

#include "gateway/app/GatewayCore.h"
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

// 组合根：主线程独占串口 Reactor/SR，HTTP 和 MQTT 各有网络循环。
// GatewayCore 独占应用逻辑；control_ 序列化发布和阻塞资源准备，查询/写入各有执行器。
class GatewayApp {
public:
    static bool blockManagedSignals();
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
    void registerSignalEvent();
    void onSerialEvent();
    void onSerialWriteEvent();
    void onDownlinkEvent();
    void onCommandTimerEvent();
    void onSignalEvent();
    void dispatchFrame(const Frame& frame);
    void applyTrackerActions(const TrackerActions& actions);
    void queueTransmission(const CommandSend& send);
    void updateSerialWriteInterest();
    void notifyDownlink();
    bool submitCommand(uint64_t id, DownCmd command);
    bool publish(std::string topic, std::string payload, int qos, std::function<void()> done);
    void requestReload();
    void reloadConfig(); // control_ 线程；仅资源交付动作回到串口 Reactor。
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
    int signal_fd_ = -1;
    // 替换和 publish 仅在 control_ 访问；启动前初始化、control_ join 后销毁。
    std::unique_ptr<MqttClient> mqtt_client_;
    std::unique_ptr<GatewayCore> core_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> reload_pending_{false};
    std::atomic<bool> http_stop_{false};
    std::atomic<bool> http_failed_{false};
    std::thread http_thread_;
    TaskExecutor control_{"gateway-control", 1024, GatewayCore::kMaxCommands + 16};
};

} // namespace gateway
