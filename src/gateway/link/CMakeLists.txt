# link 层:与 STM32 节点的链路。
#
#   NodeLink        串口收发与组帧解帧,持有 fd
#   CommandTracker  在途命令表,纯逻辑、不持有 fd,可完全单测
#
# 两者分开是为了让 CommandTracker 不被 fd 依赖拖累:验证它只需注入假时钟,
# 而验证 NodeLink 需要 socat 提供虚拟串口。
add_library(gw_link STATIC
    NodeLink.cpp
    CommandTracker.cpp
)
target_link_libraries(gw_link
    PUBLIC  gateway_includes gw_serial gw_protocol
    PRIVATE gateway_options gw_log
)
