# pipeline 层测试:帧 → 记录的业务解码
gateway_add_test(telemetry_decoder_test
    SOURCES TelemetryDecoderTest.cpp
    LIBS    gw_pipeline
    LABELS  "unit;pipeline"
)
