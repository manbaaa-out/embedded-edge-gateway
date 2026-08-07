# node-sim:STM32 传感节点模拟器。
#
# 仅依赖 edge_proto,与网关、固件使用同一份组帧解帧代码。
# 不链接网关的任何 target:模拟器扮演的是对端,复用网关实现会使一致性验证失去意义。
add_executable(node-sim main.cpp)
target_link_libraries(node-sim PRIVATE
    edge_proto
    gateway_options
)
