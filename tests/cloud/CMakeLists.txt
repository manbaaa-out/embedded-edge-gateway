# cloud 层测试:MQTT topic/payload → 下行命令的翻译与参数校验
gateway_add_test(command_translator_test
    SOURCES CommandTranslatorTest.cpp
    LIBS    gw_cloud
    LABELS  "unit;cloud"
)
