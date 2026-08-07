# core 层:日志、配置、并发原语。
#
# core 不依赖任何上层,也不含业务概念(帧、传感器、MQTT)。该约束由
# target_link_libraries 保证:本层不链接任何上层 target,越层引用会在链接期失败。

# ---- log:同步外壳 Logger + 异步双缓冲后端 AsyncLogger ----
add_library(gw_log STATIC
    log/Logger.cpp
    log/AsyncLogger.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(gw_log
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PUBLIC  Threads::Threads      # AsyncLogger 起后台 flush 线程
)

# ---- concurrent:线程安全队列 + 线程池(模板,header-only)----
add_library(gw_concurrent INTERFACE)
target_link_libraries(gw_concurrent INTERFACE gateway_includes Threads::Threads)

# ---- config:解析 + 热加载(load-then-swap)----
add_library(gw_config STATIC
    config/Config.cpp
)
target_link_libraries(gw_config
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PRIVATE gw_log                # LOG_* 仅用于 .cpp,头文件不暴露
)
