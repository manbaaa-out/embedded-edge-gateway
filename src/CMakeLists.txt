# ============================================================
# 网关源码树 —— 按职责分层,依赖方向自下而上,由 target_link_libraries 钉死。
#
#   app        装配:把下面所有层接起来(唯一能看见全部的层)
#    ├─ link       与 STM32 节点的链路:收发帧 · 命令在途表 · 超时重发
#    ├─ pipeline   遥测数据流:解码 → 落库 / 上云
#    ├─ cloud      MQTT
#    ├─ storage    SQLite
#    ├─ io         与 OS 打交道:串口 · epoll · HTTP
#    ├─ protocol   edge_proto 的薄 C++ 封装(实现在仓库顶层 protocol/)
#    └─ core       日志 · 配置 · 并发原语(不含任何业务概念)
#
# include 统一带 gateway/ 前缀(#include "gateway/link/NodeLink.h"):
# 看路径即知层级,且不会与系统头或第三方头重名。
# ============================================================

# 所有层共享同一个 include 根 = src/,于是 gateway/... 前缀自然成立
# 所有层显式 PUBLIC 链接它,于是下游(含 tests/tools)也自动看到同一个 include 根
add_library(gateway_includes INTERFACE)
target_include_directories(gateway_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# 顺序 = 依赖顺序:被依赖的层先定义
add_subdirectory(gateway/core)
add_subdirectory(gateway/protocol)
add_subdirectory(gateway/storage)
add_subdirectory(gateway/cloud)
add_subdirectory(gateway/io)
add_subdirectory(gateway/app)
