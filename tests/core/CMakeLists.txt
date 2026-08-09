# core 层测试:配置解析与热加载
gateway_add_test(config_test
    SOURCES ConfigTest.cpp
    LIBS    gw_config
    LABELS  "unit;core"
)

# 遥测数值的文本格式。formatValue 是 header-only 纯函数,只需协议头取定标常数。
gateway_add_test(number_test
    SOURCES NumberTest.cpp
    LIBS    gw_format edge_proto
    LABELS  "unit;core"
)
