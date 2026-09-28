# config_test 链接 gw_config，覆盖配置解析、统一校验与热加载差异检测。
gateway_add_test(config_test
    SOURCES ConfigTest.cpp
    LIBS    gw_config
    LABELS  "unit;core"
)

# number_test 验证 header-only 格式函数，并链接 edge_proto 取得定标常量。
gateway_add_test(number_test
    SOURCES NumberTest.cpp
    LIBS    gw_format edge_proto
    LABELS  "unit;core"
)

# 通过子进程退出时的真实异步输出验证格式化、行长边界和截断标记。
gateway_add_test(logger_test
    SOURCES LoggerTest.cpp
    LIBS    gw_log
    LABELS  "unit;core"
)
