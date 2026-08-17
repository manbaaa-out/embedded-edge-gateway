# 云端适配层：MqttClient 为头文件实现，静态库只编译命令翻译逻辑。
# MOSQUITTO_LIB 与 MOSQUITTO_INC 分别保存系统库路径和公开头文件目录。
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INC mosquitto.h REQUIRED)

# gw_cloud 对外提供命令翻译符号，并通过公共使用要求暴露 MQTT 头文件依赖。
add_library(gw_cloud STATIC
    CommandTranslator.cpp
)
target_include_directories(gw_cloud PUBLIC ${MOSQUITTO_INC})
target_link_libraries(gw_cloud
    PUBLIC  gateway_includes edge_proto ${MOSQUITTO_LIB}  # 公共接口直接使用协议与 MQTT 类型。
    PRIVATE gateway_options                              # 仅编译本目标时使用的统一选项。
    PUBLIC  gw_log                                       # 头文件内联方法会调用日志接口。
)
