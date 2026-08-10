# pipeline 层测试:帧 → 记录的业务解码
gateway_add_test(telemetry_decoder_test
    SOURCES TelemetryDecoderTest.cpp
    LIBS    gw_pipeline
    LABELS  "unit;pipeline"
)

# 落库写线程:持久化、攒批不变性、换库边界
gateway_add_test(telemetry_pipeline_test
    SOURCES TelemetryPipelineTest.cpp
    LIBS    gw_pipeline
    LABELS  "unit;pipeline"
)
