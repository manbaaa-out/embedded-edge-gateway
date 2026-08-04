# ============================================================
# pipeline 层:遥测数据流 —— 帧解码 + 双写扇出
#
# decodeTelemetry 是纯函数(帧 → 记录),不碰数据库也不碰 MQTT,
# 故本层对 storage / cloud 只在 TelemetryPipeline 一处依赖。
# ============================================================
add_library(gw_pipeline STATIC
    TelemetryDecoder.cpp
    TelemetryPipeline.cpp
)
target_link_libraries(gw_pipeline
    PUBLIC  gateway_includes gw_protocol gw_concurrent gw_storage gw_cloud
    PRIVATE gateway_options gw_log
)
