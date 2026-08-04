# protocol 层(网关侧):edge_proto 的薄 C++ 封装。
#
# 真正的协议实现在仓库顶层 protocol/ —— 与 STM32 固件共享的同一份 C99 源码。
# 本 target 只提供 vector / std::function 形态的外衣,不含任何协议知识。
add_library(gw_protocol STATIC
    FrameCodec.cpp
)
target_link_libraries(gw_protocol
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PUBLIC  edge_proto        # 头文件里就 #include 了 edge_frame.h → PUBLIC
)
