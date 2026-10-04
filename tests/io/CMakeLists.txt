# 对外 HTTP 接口：查询参数边界及真实 socket 的异步连接、超时和停机行为。
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
gateway_add_test(http_server_test
    SOURCES HttpServerTest.cpp
    LIBS    gw_http Boost::system Threads::Threads
    LABELS  "unit;io;integration"
)
