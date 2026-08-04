# core 层测试:配置解析与热加载
gateway_add_test(config_test
    SOURCES ConfigTest.cpp
    LIBS    gw_config
    LABELS  "unit;core"
)
