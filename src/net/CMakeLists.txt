# M7 Reactor + M11 HTTP + [S5/S6] HttpServer(查 SQLite + 内嵌 Web 监控页 + uPlot 曲线)

# ---------- [S6] 编译期把 web/ 静态资源嵌入二进制 ----------
# 嵌入式 asset embedding:前端(html + uPlot js/css)编进二进制,
# 单文件部署、离线可用,不依赖外部文件或 CDN。
set(WEB_DIR ${CMAKE_SOURCE_DIR}/web)
file(READ ${WEB_DIR}/index.html WEB_INDEX_HTML)
file(READ ${WEB_DIR}/uplot.js   WEB_UPLOT_JS)
file(READ ${WEB_DIR}/uplot.css  WEB_UPLOT_CSS)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/WebAsset.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/WebAsset.h
    @ONLY
)
# 任一资源改动都触发重新配置
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${WEB_DIR}/index.html
    ${WEB_DIR}/uplot.js
    ${WEB_DIR}/uplot.css
)
# ------------------------------------------------------------

add_library(net STATIC
    EventLoop.cpp
    HttpRequest.cpp
    HttpServer.cpp
)

target_include_directories(net PUBLIC .)
target_include_directories(net PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(net PRIVATE
    gateway_options
    db
)
