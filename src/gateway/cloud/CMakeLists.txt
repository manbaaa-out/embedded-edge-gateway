# 云端适配层：命令翻译和 MQTT 客户端实现统一编入静态库。
# MOSQUITTO_LIB 与 MOSQUITTO_INC 分别保存系统库路径和实现所需头文件目录。
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INC mosquitto.h REQUIRED)

# gw_cloud 对外提供命令翻译和 MQTT 客户端符号；libmosquitto 仅属于实现细节。
add_library(gw_cloud STATIC
    CommandTranslator.cpp
    MqttClient.cpp
)
target_include_directories(gw_cloud PRIVATE ${MOSQUITTO_INC})
target_link_libraries(gw_cloud
    PUBLIC  gateway_includes edge_proto  # 公开命令类型直接使用共享协议定义。
    PRIVATE gateway_options gw_log ${MOSQUITTO_LIB}  # MQTT 与日志只用于实现。
)
