# gw_protocol 是网关侧 C++ 容器/回调适配层；线上协议仍由 edge_proto 唯一实现。
add_library(gw_protocol STATIC
    FrameCodec.cpp
)
# FrameCodec.h 直接包含 edge_proto 头文件，因此 edge_proto 必须作为 PUBLIC 依赖传播。
target_link_libraries(gw_protocol
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PUBLIC  edge_proto
)
