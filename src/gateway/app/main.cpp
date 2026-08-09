#include "gateway/core/log/Logger.h"
#include "gateway/core/config/Config.h"
#include "gateway/app/GatewayApp.h"

#include <exception>

int main(int argc, char* argv[]) {
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

    // 必须早于任何线程的创建,原因见 GatewayApp::blockManagedSignals 的声明。
    // 失败不致命:仅热加载与优雅停机不可用,采集照常。
    (void) gateway::GatewayApp::blockManagedSignals();

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
        const int rc = app.run();   // 阻塞至 SIGTERM / SIGINT
        // app 在此析构(停 HTTP 线程、drain 线程池、关连接),随后 main 返回,
        // 静态对象析构,AsyncLogger 把缓冲里剩下的日志刷出去。
        // 所以这里必须先取到 rc 再返回,不能写成 return app.run() ——
        // 那样也能正确析构,只是把「返回之后还有事发生」这件事藏起来了。
        return rc;
    } catch (const std::exception& e) {
        // 串口打不开 / broker 连不上 / 磁盘异常等在此统一致命退出
        LOG_ERROR("gateway fatal: %s", e.what());
        return 1;
    }
}
