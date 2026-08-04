#include "gateway/core/log/Logger.h"
#include "gateway/core/config/Config.h"             // [M2] 配置初始化
#include "gateway/app/GatewayApp.h"

#include <exception>

int main(int argc, char* argv[]) {
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

    // ============================================================
    // 【第一件事】屏蔽 SIGHUP —— 必须早于任何线程的创建。
    //
    // 信号掩码按线程生效并在创建时继承。GatewayApp 的构造函数会起 HTTP 线程
    // 与线程池 worker,startMqtt 还会起 mosquitto 网络线程;这些线程若带着
    // 「未屏蔽 SIGHUP」的掩码跑起来,kill -HUP 就可能落到它们头上,
    // 按默认行为把整个进程杀掉 —— 热加载于是表现为「时灵时不灵」。
    // 屏蔽失败不致命:大不了不能热加载,采集照常。
    // ============================================================
    (void) gateway::GatewayApp::blockReloadSignal();

    // ============================================================
    // [M2] 配置初始化。失败致命:连配置都读不出,无法决定怎么启动。
    //      (对照下面 GatewayApp 资源异常=致命:配置是核心前提,热加载是辅助便利)
    // ============================================================
    try {
        gateway::ConfigManager::init(conf_path);
    } catch (const std::exception& e) {
        LOG_ERROR("config init failed: %s", e.what());
        return 1;
    }
    auto cfg = gateway::ConfigManager::current();   // 启动配置快照
    gateway::Logger::setLevel(static_cast<gateway::LogLevel>(cfg->log_level));
    LOG_INFO("gateway starting, config from %s", conf_path);

    try {
        gateway::GatewayApp app;   // 构造:打开 db/client/roDb+http 线程/pool/port
        return app.run();          // 装配四类事件源 + 主循环(永久阻塞)
    } catch (const std::exception& e) {
        // 串口打不开 / broker 连不上 / 磁盘异常等统一在此致命退出
        LOG_ERROR("gateway fatal: %s", e.what());
        return 1;
    }
}
