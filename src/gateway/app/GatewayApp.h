#pragma once

/**
 * @file GatewayApp.h
 * @brief 定义网关进程的组合根和各运行组件的生命周期边界。
 *
 * GatewayApp 不实现帧协议、命令翻译或持久化算法，而是把这些组件装配成一个进程。
 * 主线程以 EventLoop 串行处理串口收帧、MQTT 下行唤醒、命令超时和 Unix 信号，
 * 因而 NodeLink 与 CommandTracker 无需跨线程加锁。MQTT、HTTP 和数据库写线程只通过
 * MqttClient 的线程安全接口、ThreadSafeQueue 或只读数据库连接与主线程协作。
 *
 * 成员的声明顺序同时定义构造顺序和逆序析构顺序。调整成员前必须确认后台线程会在
 * 它所依赖的连接、队列和回调目标之前停止。
 */

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

/**
 * @brief 持有进程级资源并驱动网关主事件循环。
 *
 * 设计上把所有串口写操作收敛到主线程：MQTT 网络线程只生成 DownCmd 并写 eventfd，
 * 主线程被唤醒后再组帧和发送。这既避免串口帧交错，也让序列号分配、ACK 销账和重试
 * 共用同一条时间线。
 */
class GatewayApp {
public:
    /**
     * @brief 屏蔽由主循环接管的进程信号。
     *
     * 必须在线程创建前调用，使后续线程继承同一信号掩码。run() 随后通过 signalfd
     * 在主线程处理 SIGHUP、SIGTERM 和 SIGINT。
     *
     * @return 屏蔽成功时为 true；失败时为 false，此时 signalfd 机制不可依赖。
     */
    static bool blockManagedSignals();

    /**
     * @brief 根据 ConfigManager 的当前快照创建 MQTT、串口、数据库和 HTTP 资源。
     * @pre ConfigManager::init() 已成功调用。
     * @throws std::exception 任一启动必需资源创建失败时向进程入口传播异常。
     */
    GatewayApp();

    /**
     * @brief 通知 HTTP 服务退出并等待其线程结束，随后由成员析构完成其余清理。
     */
    ~GatewayApp();

    /** 应用对象独占线程和文件描述符；other 表示禁止复制的源对象。 */
    GatewayApp(const GatewayApp& /* other */) = delete;

    /** 资源所有权不能复制；other 表示禁止赋值的源对象。 */
    GatewayApp& operator=(const GatewayApp& /* other */) = delete;

    /**
     * @brief 注册全部事件源并阻塞运行主事件循环。
     * @return 0 表示主循环正常退出；1 表示核心下行唤醒通道创建失败。
     */
    int run();

private:
    /** @brief 把当前 NodeLink 的串口描述符以边沿触发方式注册到主循环。 */
    void setupSerial();

    /**
     * @brief 创建连接 MQTT 网络线程与主线程的 eventfd 通知通道。
     * @return 通道创建并注册成功时为 true，否则为 false。
     */
    bool setupDownlink();

    /**
     * @brief 为当前 MQTT 客户端安装命令回调、登记订阅并启动网络循环。
     *
     * 网络循环启动失败时仅记录错误并返回；该函数不抛出该类运行期错误。
     */
    void startMqtt();

    /** @brief 创建周期 timerfd，用于驱动 CommandTracker 的超时重试和失败判定。 */
    void setupCmdTimer();

    /** @brief 创建 signalfd，把热加载和退出信号纳入主事件循环。 */
    void setupSignal();

    /**
     * @brief 按帧类型把串口上行分派给 ACK 处理或遥测处理。
     * @param f 已通过 CRC 校验的完整协议帧；仅在主线程回调中使用。
     */
    void onFrame(const Frame& f);

    /**
     * @brief 校验 ACK/查询响应、销账对应序列号，并发布业务结果到 MQTT。
     * @param f 类型为 EDGE_TYPE_ACK 或 EDGE_TYPE_QUERY_RESP 的协议帧。
     */
    void onAckFrame(const Frame& f);

    /** @brief 消费 eventfd 计数并排空 MQTT 网络线程提交的下行命令队列。 */
    void onDownlinkWakeup();

    /** @brief 消费 timerfd 计数，发送到期重试并发布最终超时结果。 */
    void onCmdTimer();

    /**
     * @brief 重新解析配置，并按差异独立重建数据库、MQTT 和串口资源。
     *
     * 单项重建失败只记录降级状态，不撤销同一次重载中已经成功的其他项目。
     */
    void reloadConfig();

    /**
     * @brief 为 NodeLink 当前持有的串口 fd 创建非拥有型 Channel。
     * @return 可注册到 EventLoop 的共享 Channel；fd 的关闭权仍属于 SerialPort。
     */
    std::shared_ptr<channel> makeSerialChannel();

    /** 当前生效的 MQTT 连接；热加载成功构造新连接后整体替换该对象。 */
    std::shared_ptr<MqttClient> client_;

    /**
     * HTTP 当前只读 SQLite 连接；运行期仅通过 shared_ptr 原子自由函数发布和取得。
     * 每个请求持有自己的副本，因此换库不会使正在执行的查询失去连接。
     */
    std::shared_ptr<Database>   roDb_;

    /** 跨线程停机标志；析构线程写入，HTTP 线程周期读取。 */
    std::atomic<bool>           http_stop_{false};

    /** 运行监控 HTTP 服务的线程；必须在析构函数体中 join。 */
    std::thread                 http_thread_;

    /** 主线程独占的 STM32 串口链路，负责收帧、组帧和写串口。 */
    std::unique_ptr<NodeLink>          link_;

    /** 遥测异步落库管线；optional 允许在数据库创建后再就地启动写线程。 */
    std::optional<TelemetryPipeline>   pipeline_;

    /** 主线程独占的在途命令表，保存序列号、重试次数和发送时刻。 */
    CommandTracker                     tracker_;

    /** MQTT 网络线程生产、主线程消费的下行命令队列；默认容量为无限。 */
    ThreadSafeQueue<DownCmd>           cmd_queue_;

    /** 主线程 Reactor，统一派发串口、eventfd、timerfd 和 signalfd。 */
    EventLoop                loop_;

    /** 当前串口 fd 对应的 Channel；串口热加载时随 fd 一起替换。 */
    std::shared_ptr<channel> serial_channel_;

    /** MQTT 下行队列的 eventfd；注册后由拥有型 Channel 负责关闭。 */
    int evfd_        = -1;

    /** 命令超时扫描的 timerfd；注册后由拥有型 Channel 负责关闭。 */
    int cmd_timerfd_ = -1;

    /** 接收 SIGHUP/SIGTERM/SIGINT 的 signalfd；由拥有型 Channel 负责关闭。 */
    int sfd_         = -1;
};

}  // namespace gateway
