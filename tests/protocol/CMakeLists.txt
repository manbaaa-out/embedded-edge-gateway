# 协议测试按三层拆分：共享 C 核心的性质、跨仓库金标准，以及网关 C++ 封装；
# 分开建目标后，失败名称可以直接定位契约层级。

# protocol_contract_test 直接链接 edge_proto，以性质测试覆盖共享 C 核心。
gateway_add_test(protocol_contract_test
    SOURCES ContractTest.cpp
    LIBS    edge_proto
    LABELS  "unit;protocol"
)

# protocol_conformance_test 读取共享 CSV 金标准；conformance 标签供 CI 单独筛选。
gateway_add_test(protocol_conformance_test
    SOURCES ConformanceTest.cpp
    LIBS    edge_proto
    LABELS  "unit;protocol;conformance"
)

# frame_codec_test 链接 gw_protocol，验证网关 C++ 封装的长度边界和透明性。
gateway_add_test(frame_codec_test
    SOURCES FrameCodecTest.cpp
    LIBS    gw_protocol
    LABELS  "unit;protocol"
)
