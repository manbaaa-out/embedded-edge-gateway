# telemetry_decoder_test 链接 gw_pipeline，测试帧到 Reading 的纯函数映射。
gateway_add_test(telemetry_decoder_test
    SOURCES TelemetryDecoderTest.cpp
    LIBS    gw_pipeline
    LABELS  "unit;pipeline"
)

# telemetry_pipeline_test 使用真实临时 SQLite 文件验证异步写入、批处理和换库顺序。
gateway_add_test(telemetry_pipeline_test
    SOURCES TelemetryPipelineTest.cpp
    LIBS    gw_pipeline
    LABELS  "unit;pipeline"
)
