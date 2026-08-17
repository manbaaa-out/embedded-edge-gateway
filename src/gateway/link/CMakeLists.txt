# STM32 链路层：NodeLink 管理串口 I/O，CommandTracker 管理无 I/O 的命令时序。
# 公共头文件暴露 SerialPort/Frame 类型，故 gw_serial 与 gw_protocol 向消费者传播；
# 编译选项和实现日志只在本静态库内部使用。
add_library(gw_link STATIC
    NodeLink.cpp
    CommandTracker.cpp
)
target_link_libraries(gw_link
    PUBLIC  gateway_includes gw_serial gw_protocol
    PRIVATE gateway_options gw_log
)
