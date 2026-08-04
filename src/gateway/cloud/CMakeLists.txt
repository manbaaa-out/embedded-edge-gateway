# ============================================================
# cloud 层:MQTT 客户端 + 下行命令翻译
#
# translateCommand 是纯函数(topic + payload → 命令),不碰 mosquitto,
# 所以它能被单测 —— 而重构前它长在 mosquitto 回调 lambda 里,测不了。
# ============================================================
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INC mosquitto.h REQUIRED)

add_library(gw_cloud STATIC
    CommandTranslator.cpp
)
target_include_directories(gw_cloud PUBLIC ${MOSQUITTO_INC})
target_link_libraries(gw_cloud
    PUBLIC  gateway_includes edge_proto ${MOSQUITTO_LIB}
    PRIVATE gateway_options
)
