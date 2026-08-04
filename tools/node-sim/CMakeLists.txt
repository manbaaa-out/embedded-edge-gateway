# ============================================================
# node-sim:STM32 传感节点模拟器
#
# 只依赖 edge_proto —— 与网关、与固件用的是同一份组帧解帧代码。
# 刻意【不】链接网关的任何 target:模拟器要扮演的是对端,
# 若它复用网关的实现,"两端能对上"就成了自说自话。
# ============================================================
add_executable(node-sim main.cpp)
target_link_libraries(node-sim PRIVATE
    edge_proto
    gateway_options
)
