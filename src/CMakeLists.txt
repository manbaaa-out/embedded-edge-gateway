# 网关源码树,按职责分层。依赖方向自下而上,由 target_link_libraries 约束。
#
#   app        装配层,唯一可见全部下层
#    ├─ link       与 STM32 节点的链路:收发帧、命令在途表、超时重发
#    ├─ pipeline   遥测数据流:解码 → 落库 / 上云
#    ├─ cloud      MQTT
#    ├─ storage    SQLite
#    ├─ io         操作系统交互:串口、epoll、HTTP
#    ├─ protocol   edge_proto 的 C++ 封装,实现在仓库顶层 protocol/
#    └─ core       日志、配置、并发原语,不含业务概念
#
# include 统一带 gateway/ 前缀,使路径即体现层级,并避免与系统头或第三方头重名。

# 所有层共享 src/ 作为 include 根,并以 PUBLIC 链接本 target,
# 使下游(含 tests / tools)看到同一个 include 根
add_library(gateway_includes INTERFACE)
target_include_directories(gateway_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# 顺序即依赖顺序:被依赖的层先定义
add_subdirectory(gateway/core)
add_subdirectory(gateway/protocol)
add_subdirectory(gateway/storage)
add_subdirectory(gateway/cloud)
add_subdirectory(gateway/io)
add_subdirectory(gateway/link)
add_subdirectory(gateway/pipeline)
add_subdirectory(gateway/app)
