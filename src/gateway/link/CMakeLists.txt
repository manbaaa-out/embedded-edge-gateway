# ============================================================
# link 层:与 STM32 节点的链路
#
#   NodeLink        串口收发 + 组帧解帧(碰 fd)
#   CommandTracker  在途命令表(纯逻辑,零 fd,可完全单测)
#
# 两者刻意分开:碰 fd 的部分测起来要 socat,不碰 fd 的部分注入假时钟就能测遍。
# 把它们揉在一起,后者就会被前者拖下水 —— 这正是重构前的状况。
# ============================================================
add_library(gw_link STATIC
    NodeLink.cpp
    CommandTracker.cpp
)
target_link_libraries(gw_link
    PUBLIC  gateway_includes gw_serial gw_protocol
    PRIVATE gateway_options gw_log
)
