# 操作系统 I/O 适配分为三个静态目标，避免上层按需链接时引入无关实现。

# gw_serial：串口 fd 生命周期和 termios 配置。
add_library(gw_serial STATIC
    serial/SerialPort.cpp
)
target_link_libraries(gw_serial
    PUBLIC  gateway_includes          # 使用方可包含 SerialPort.h。
    PRIVATE gateway_options gw_log    # 本目标的编译策略与实现日志。
)

# gw_event：epoll Reactor 和 channel 生命周期。
add_library(gw_event STATIC
    event/EventLoop.cpp
)
target_link_libraries(gw_event
    PUBLIC  gateway_includes          # 使用方可包含 EventLoop.h。
    PRIVATE gateway_options gw_log    # 本目标的编译策略与实现日志。
)

# WEB_DIR 是自有页面与第三方 uPlot 压缩资源的配置期输入目录。
set(WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/http/assets)
# 三个 WEB_* 变量保存资源原文，供 WebAsset.h.in 的 @VAR@ 占位符展开。
file(READ ${WEB_DIR}/index.html WEB_INDEX_HTML)
file(READ ${WEB_DIR}/uplot.js   WEB_UPLOT_JS)
file(READ ${WEB_DIR}/uplot.css  WEB_UPLOT_CSS)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/http/WebAsset.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/gateway/io/http/WebAsset.h
    @ONLY  # 只展开 @VAR@，避免前端中的 ${...} 被 CMake 解释。
)
# 将资源登记为配置依赖，使任一内容变化都重新生成嵌入头文件。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${WEB_DIR}/index.html
    ${WEB_DIR}/uplot.js
    ${WEB_DIR}/uplot.css
)

# gw_http：增量请求解析、静态资源路由和只读查询服务。
add_library(gw_http STATIC
    http/HttpRequest.cpp
    http/HttpServer.cpp
)
# WebAsset.h 仅供 HttpServer.cpp 使用，因此生成目录不向使用方传播。
target_include_directories(gw_http PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gw_http
    PUBLIC  gateway_includes                 # 使用方可包含 HttpServer.h。
    PRIVATE gateway_options                  # 本目标统一编译策略。
    PUBLIC  gw_event                         # 当前构建图向使用方传递事件循环目标。
    PRIVATE gw_storage gw_log gw_format      # 数据查询、日志和 JSON 数值格式。
)
