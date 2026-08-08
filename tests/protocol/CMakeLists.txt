# 协议层测试:契约性质与金标准向量一致性。
#
# 这两个 target 失败意味着网关与 STM32 节点对同一串字节的解释已经分叉。

gateway_add_test(protocol_contract_test
    SOURCES ContractTest.cpp
    LIBS    edge_proto
    LABELS  "unit;protocol"
)

gateway_add_test(protocol_conformance_test
    SOURCES ConformanceTest.cpp
    LIBS    edge_proto
    LABELS  "unit;protocol;conformance"
)

# 上面两个测的是与固件共享的 C 核心,本 target 测的是网关独有的 C++ 封装层,
# 故链接 gw_protocol 而非 edge_proto。
gateway_add_test(frame_codec_test
    SOURCES FrameCodecTest.cpp
    LIBS    gw_protocol
    LABELS  "unit;protocol"
)
