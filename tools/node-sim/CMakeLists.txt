# node-sim 由单一入口源构建；edge_proto 提供与固件相同的字节协议，gateway_options
# 让工具源码遵守项目警告基线。它不链接网关业务库，避免同源错误使 e2e 假通过。
add_executable(node-sim main.cpp)
target_link_libraries(node-sim PRIVATE
    edge_proto
    gateway_options
)
