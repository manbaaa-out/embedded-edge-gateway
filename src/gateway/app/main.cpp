/**
 * @file main.cpp
 * @brief 建立信号和配置前提，并把进程生命周期交给 GatewayApp。
 *
 * 入口刻意不包含业务分支：先屏蔽托管信号，再初始化不可缺少的配置，最后创建应用
 * 组合根。这个顺序保证随后创建的日志、MQTT、数据库和 HTTP 线程继承信号掩码。
 */

#include "gateway/core/log/Logger.h"
#include "gateway/core/config/Config.h"
#include "gateway/app/GatewayApp.h"

#include <exception>

/**
 * @brief 网关进程入口。
 * @param argc 命令行参数数量；大于 1 时读取自定义配置路径。
 * @param argv 参数数组；argv[1] 可指定配置文件，其他参数当前忽略。
 * @return 0 表示正常停机，1 表示配置、启动资源或核心事件源失败。
 */
int main(int argc, char* argv[]) {
    // conf_path 指向进程参数存储或字符串常量，生命周期覆盖整个 main()。
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

    // 屏蔽失败不会阻止采集启动，但 signalfd 热加载和优雅退出将不可依赖。
    (void) gateway::GatewayApp::blockManagedSignals();

    try {
        gateway::ConfigManager::init(conf_path);
    } catch (const std::exception& e) {
        // e 保存配置文件打开、解析或校验失败的具体原因。
        LOG_ERROR("config init failed: %s", e.what());
        return 1;
    }

    // cfg 是启动配置的不可变共享快照，用于在首次业务日志前设置过滤级别。
    auto cfg = gateway::ConfigManager::current();
    gateway::Logger::setLevel(static_cast<gateway::LogLevel>(cfg->log_level));
    LOG_INFO("gateway starting, config from %s", conf_path);

    try {
        // app 是所有运行期资源的组合根；其块作用域明确限定完整析构发生在 main 返回前。
        gateway::GatewayApp app;

        // rc 是主事件循环给出的进程退出码，先保存以便 app 在 return 前完成析构。
        const int rc = app.run();
        return rc;
    } catch (const std::exception& e) {
        // e 汇总启动资源或主事件循环抛出的致命异常。
        LOG_ERROR("gateway fatal: %s", e.what());
        return 1;
    }
}
