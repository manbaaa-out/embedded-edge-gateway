# pipeline 层:遥测数据流,含帧解码与双写扇出。
#
# decodeTelemetry 是纯函数,不依赖数据库与 MQTT,故本层对 storage / cloud
# 的依赖集中在 TelemetryPipeline 一处。
add_library(gw_pipeline STATIC
    TelemetryDecoder.cpp
    TelemetryPipeline.cpp
)
target_link_libraries(gw_pipeline
    PUBLIC  gateway_includes gw_protocol gw_concurrent gw_storage gw_cloud
    PRIVATE gateway_options gw_log
)
