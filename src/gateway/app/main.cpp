#include "gateway/core/log/Logger.h"
#include "gateway/core/config/Config.h"
#include "gateway/app/GatewayApp.h"

#include <exception>

int main(int argc, char* argv[]) {
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

    // 必须早于任何线程的创建,原因见 GatewayApp::blockReloadSignal 的声明。
    // 失败不致命:仅热加载不可用,采集照常。
    (void) gateway::GatewayApp::blockReloadSignal();

    // 配置初始化。失败致命:配置读不出就无法决定如何启动。
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
        gateway::GatewayApp app;
        return app.run();   // 永久阻塞
    } catch (const std::exception& e) {
        // 串口打不开 / broker 连不上 / 磁盘异常等在此统一致命退出
        LOG_ERROR("gateway fatal: %s", e.what());
        return 1;
    }
}
