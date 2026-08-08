# io 层测试:HTTP 请求解析,覆盖半包、大小写、pipelining 与长连接复用
gateway_add_test(http_request_test
    SOURCES HttpRequestTest.cpp
    LIBS    gw_http
    LABELS  "unit;io"
)

# 查询参数归一:请求带的 n 与配置给的默认值之间的边界
gateway_add_test(http_server_test
    SOURCES HttpServerTest.cpp
    LIBS    gw_http
    LABELS  "unit;io"
)
