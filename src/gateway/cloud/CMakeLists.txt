# cloud 层:MQTT 客户端与下行命令翻译。
#
# translateCommand 是纯函数,不依赖 mosquitto,可独立单测。
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INC mosquitto.h REQUIRED)

add_library(gw_cloud STATIC
    CommandTranslator.cpp
)
target_include_directories(gw_cloud PUBLIC ${MOSQUITTO_INC})
target_link_libraries(gw_cloud
    PUBLIC  gateway_includes edge_proto ${MOSQUITTO_LIB}
    PRIVATE gateway_options
    PUBLIC  gw_log                # MqttClient.h 是 header-only,LOG_* 随头文件外传
)
