# ============================================================
# io 层:与操作系统打交道的部分 —— 串口 · epoll 事件循环 · HTTP
#
# 只依赖 core,不依赖 link/pipeline/cloud:
# io 层不知道自己搬运的字节是什么业务含义。
# ============================================================

# ---- serial:termios RAII 封装(读 + 写)----
add_library(gw_serial STATIC
    serial/SerialPort.cpp
)
target_link_libraries(gw_serial PUBLIC gateway_includes PRIVATE gateway_options)

# ---- event:epoll Reactor + channel 抽象 ----
# channel 把 socket / timerfd / signalfd / eventfd 统一成同一种东西,
# 这是整个网关「单线程统一事件驱动」的地基。
add_library(gw_event STATIC
    event/EventLoop.cpp
)
target_link_libraries(gw_event PUBLIC gateway_includes PRIVATE gateway_options)

# ---- http:内嵌监控页 + 只读查询接口 ----
# 编译期把 http/assets/ 下的静态资源嵌进二进制:单文件部署、离线自包含、不依赖 CDN。
#
# 资源与消费它的模块放在一起,而不是仓库根的 web/ —— 那个位置会让人以为
# 这是个独立的前端工程,实际上它连构建产物都不是,而是本模块的【编译期输入】:
# 改了 index.html 就得重新 configure(见下面的 CMAKE_CONFIGURE_DEPENDS)。
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
