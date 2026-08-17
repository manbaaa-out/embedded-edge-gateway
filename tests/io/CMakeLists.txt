# http_request_test 链接 gw_http，覆盖请求分片、流水线和长连接复用状态。
gateway_add_test(http_request_test
    SOURCES HttpRequestTest.cpp
    LIBS    gw_http
    LABELS  "unit;io"
)

# http_server_test 同样链接 gw_http，但只验证查询条数归一的无 I/O 边界函数。
gateway_add_test(http_server_test
    SOURCES HttpServerTest.cpp
    LIBS    gw_http
    LABELS  "unit;io"
)
