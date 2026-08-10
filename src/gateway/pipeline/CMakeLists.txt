# pipeline 层:遥测数据流,含帧解码与落库写线程。
#
# decodeTelemetry 是纯函数,不依赖数据库;本层对 storage 的依赖集中在
# TelemetryPipeline 一处。
#
# 本层不再依赖 gw_cloud:上云已收归 app 层直接调用 MqttClient::publish
# (它无锁且不阻塞,没有理由再经一层线程)。依赖图少一条边,正是那次改动
# 在构建系统上的投影。
add_library(gw_pipeline STATIC
    TelemetryDecoder.cpp
    TelemetryPipeline.cpp
)
target_link_libraries(gw_pipeline
    PUBLIC  gateway_includes gw_protocol gw_concurrent gw_storage
    PRIVATE gateway_options gw_log
)
