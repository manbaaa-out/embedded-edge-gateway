# 基础设施层：日志、并发工具、数值格式和配置管理，不依赖业务层 target。

# gw_log 编译同步格式化前端与异步写线程。Threads 是最终链接所需的传递依赖，
# gateway_options 只影响本目标编译，不强加给消费者。
add_library(gw_log STATIC
    log/Logger.cpp
    log/AsyncLogger.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(gw_log
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PUBLIC  Threads::Threads
)

# gw_concurrent 没有源文件；INTERFACE target 传播 include 路径和线程运行库需求。
add_library(gw_concurrent INTERFACE)
target_link_libraries(gw_concurrent INTERFACE gateway_includes Threads::Threads)

# gw_format 只传播 header-only formatValue()，独立 target 让格式契约的使用者显式可见。
add_library(gw_format INTERFACE)
target_link_libraries(gw_format INTERFACE gateway_includes)

# gw_config 的公开头文件只依赖 gateway_includes；实现中的 LOG_* 使 gw_log 保持私有依赖。
add_library(gw_config STATIC
    config/Config.cpp
)
target_link_libraries(gw_config
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PRIVATE gw_log
)
