#include "Logger.h"
#include "Config.h"             // [M2] 配置初始化
#include "GatewayApp.h"

#include <exception>

int main(int argc, char* argv[]) {
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

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
