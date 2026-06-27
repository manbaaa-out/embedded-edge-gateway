#pragma once

// ============================================================
// GatewayApp:网关应用对象。把原 main.cpp 里散落的资源与装配收成一个类。
//   - 资源(db/client/port/loop/...)作为成员,各 channel 工厂改成成员方法,
//     lambda 捕获 this —— 消除原来工厂函数 5~8 个引用参数的传递噪声。
//   - 行为与原 main.cpp 字节级一致,仅做「引用参数→成员、捕获→this」的机械平移。
//
// 【铁律】成员声明顺序 == 原 main() 局部声明顺序 —— 成员逆序析构,
//   保证 pool_ 先于 db_/client_ 析构(线程池 drain 在飞 task 时,
//   task 经 shared_ptr 快照持有的 db/client 仍存活,无 UAF)。不可重排!
// ============================================================

#include "Database.h"
#include "MqttClient.h"
#include "SerialPort.h"
#include "ThreadPool.h"
#include "ThreadSafeQueue.h"
#include "EventLoop.h"
#include "FrameParser.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace gateway {

// ------------------------------------------------------------
// 下行命令:从 MQTT(mosquitto 线程)投递给主线程统一发送。
// ------------------------------------------------------------
struct DownCmd {
    uint8_t type;                  // M5 TYPE(0x20/0x21/0x22)
    std::vector<uint8_t> arg;      // 命令参数(不含 seq,seq 由主线程发送时分配)
};

// 在途命令表元素:只在主线程访问(发命令/收 ACK/超时扫描三者同线程)→ 无需加锁。
struct InflightCmd {
    uint8_t type;                  // 命令类型(重发要用)
    std::vector<uint8_t> arg;      // 命令参数(不含 seq,重发要用)
    time_t  sent_time;             // 发送时间(超时判断)
    int     retry_count;           // 已重试次数
};

class GatewayApp {
public:
    // 构造:读 ConfigManager::current() 启动快照,打开 db/client/roDb+http 线程/pool/port。
    //   (ConfigManager::init 仍由 main 负责:配置读不出是更早的致命错。)
    GatewayApp();

    GatewayApp(const GatewayApp&)            = delete;   // 持 unique_ptr/thread/EventLoop,不可拷贝
    GatewayApp& operator=(const GatewayApp&) = delete;

    // 装配四类事件源 + 跑主循环(永久阻塞)。返回进程退出码(eventfd 建不起来=1)。
    int run();

private:
    // ---- data-path / 下行 / 热加载:由原自由函数平移成员化 ----
    void handleAck(const Frame& f);                          // 0x05/0x06 应答配对 + 销账 + 发 MQTT
    FrameParser::OnFrameCallback makeFrameHandler();         // 解析回调:分流 + 双写
    MqttClient::MessageHandler   makeDownlinkHandler();      // MQTT 下行 handler(mosquitto 线程)
    std::shared_ptr<channel>     makeSerialChannel();        // 串口 Channel(ET)
    std::shared_ptr<channel>     makeEventfdChannel();       // eventfd Channel(下行投递)
    std::shared_ptr<channel>     makeCmdTimerChannel();      // timerfd Channel(超时重试)
    void reloadConfig();                                     // SIGHUP 热加载

    // ---- run() 的装配步骤(把原 main() 的一长串拆开)----
    void setupSerial();      // 串口 Channel
    bool setupDownlink();    // eventfd + 下行 Channel(失败致命,返回 false)
    void startMqtt();        // 挂下行 handler + 订阅 + loopStart
    void setupCmdTimer();    // 超时重试 timerfd(失败降级)
    void setupSignal();      // SIGHUP signalfd(失败降级)

    // ====================================================
    // 成员顺序 == 原 main() 局部顺序(逆序析构铁律,见文件头)
    // ====================================================
    std::shared_ptr<Database>    db_;              // 上行写连接(线程池 task 持快照)
    std::shared_ptr<MqttClient>  client_;          // 上行/应答 MQTT
    std::shared_ptr<Database>    roDb_;             // HTTP 只读连接(WAL 读写并发)
    std::thread                  http_thread_;      // HTTP 监控线程(detach,捕获 roDb_ 副本)
    std::unique_ptr<ThreadPool>  pool_;             // 双写线程池(最先析构,drain 在飞 task)
    std::shared_ptr<SerialPort>  port_;             // 串口(单一写者:只主线程写)

    ThreadSafeQueue<DownCmd>     cmd_queue_;        // mosquitto 线程 push / 主线程 try_pop
    std::map<uint8_t, InflightCmd> inflight_;       // 在途命令表(仅主线程访问,无锁)
    uint8_t                      seq_counter_ = 0;  // seq 分配器(仅主线程递增)

    EventLoop                    loop_;             // 主 Reactor
    FrameParser                  parser_;           // 串口 8 状态解析机
    std::shared_ptr<channel>     serial_channel_;   // 串口 Channel(热加载会换)

    int evfd_        = -1;   // 下行唤醒(close 由 channel RAII 负责)
    int cmd_timerfd_ = -1;   // 超时扫描
    int sfd_         = -1;   // SIGHUP signalfd
};

}  // namespace gateway
