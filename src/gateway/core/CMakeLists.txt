# ============================================================
# core 层:日志 · 配置 · 并发原语
#
# 【分层铁律】core 不依赖任何上层,也不含任何业务概念 ——
# 它不知道什么是「帧」「传感器」「MQTT」。这条由 target_link_libraries
# 钉死:本层没链任何上层 target,越层 #include 会直接编译失败。
# ============================================================

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
    PRIVATE gw_log                # 只在 .cpp 里用 LOG_*,头文件不暴露 → PRIVATE
)
