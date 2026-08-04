# cloud 层:MQTT 客户端(上行发布 + 下行订阅)与命令翻译
add_library(gw_cloud INTERFACE)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INC mosquitto.h REQUIRED)
target_include_directories(gw_cloud INTERFACE ${MOSQUITTO_INC})
target_link_libraries(gw_cloud INTERFACE gateway_includes ${MOSQUITTO_LIB})
