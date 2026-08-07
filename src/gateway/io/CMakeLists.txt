# io 层:操作系统交互,包含串口、epoll 事件循环与 HTTP。
#
# 仅依赖 core,不依赖 link / pipeline / cloud:本层不感知所搬运字节的业务含义。

# ---- serial:termios 的 RAII 封装 ----
add_library(gw_serial STATIC
    serial/SerialPort.cpp
)
target_link_libraries(gw_serial PUBLIC gateway_includes PRIVATE gateway_options)

# ---- event:epoll Reactor 与 channel 抽象 ----
# channel 把 socket / timerfd / signalfd / eventfd 统一为同一种事件源,
# 是单线程统一事件驱动的基础。
add_library(gw_event STATIC
    event/EventLoop.cpp
)
target_link_libraries(gw_event PUBLIC gateway_includes PRIVATE gateway_options)

# ---- http:内嵌监控页与只读查询接口 ----
# 编译期把 http/assets/ 下的静态资源嵌入二进制,实现单文件部署与离线自包含。
#
# 资源与消费它的模块同目录存放而非置于仓库根的 web/:它不是独立前端工程,
# 而是本模块的编译期输入,修改后需重新 configure(见下方 CMAKE_CONFIGURE_DEPENDS)。
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/http/assets)
file(READ ${WEB_DIR}/index.html WEB_INDEX_HTML)
file(READ ${WEB_DIR}/uplot.js   WEB_UPLOT_JS)
file(READ ${WEB_DIR}/uplot.css  WEB_UPLOT_CSS)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/http/WebAsset.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/gateway/io/http/WebAsset.h
    @ONLY
)
# 任一资源改动都触发重新配置
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${WEB_DIR}/index.html
    ${WEB_DIR}/uplot.js
    ${WEB_DIR}/uplot.css
)

add_library(gw_http STATIC
    http/HttpRequest.cpp
    http/HttpServer.cpp
)
target_include_directories(gw_http PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gw_http
    PUBLIC  gateway_includes
    PRIVATE gateway_options
    PUBLIC  gw_event              # HttpServer.h 里出现 EventLoop / channel
    PRIVATE gw_storage gw_log
)
