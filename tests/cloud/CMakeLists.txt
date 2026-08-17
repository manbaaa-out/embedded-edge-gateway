# command_translator_test 编译唯一测试源，链接 gw_cloud 取得翻译器及 edge_proto 契约；
# unit/cloud 标签分别支持全体单测和云端适配层筛选。
gateway_add_test(command_translator_test
    SOURCES CommandTranslatorTest.cpp
    LIBS    gw_cloud
    LABELS  "unit;cloud"
)
