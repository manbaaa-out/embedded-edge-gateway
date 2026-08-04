# 协议层测试:契约性质 + 金标准向量一致性
#
# 这两个 target 是整个仓库最不该失败的测试 —— 它们红了,
# 意味着网关与 STM32 节点对同一串字节的理解已经分叉。

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
