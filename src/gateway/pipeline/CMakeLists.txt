# gw_pipeline 对外提供无状态遥测解码和单写线程落库流水线。
add_library(gw_pipeline STATIC
    TelemetryDecoder.cpp
    TelemetryPipeline.cpp
)
target_link_libraries(gw_pipeline
    PUBLIC  gateway_includes gw_protocol gw_concurrent gw_storage  # 公共头文件直接使用这些类型。
    PRIVATE gateway_options gw_log                                 # 实现侧编译选项与日志。
)
