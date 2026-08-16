cmake_minimum_required(VERSION 3.21)   # 3.21+:CMakePresets v3
project(gateway VERSION 1.0.0 LANGUAGES C CXX)   # C 用于共享协议核心 edge_proto

# 顶层只描述项目由哪些部分组成以及它们如何装配。
# 编译选项见 cmake/CompilerWarnings.cmake,构建形态见 CMakePresets.json。

# ---- 1. Build type 防御 ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
    message(STATUS "BUILD_TYPE 未指定,默认 Debug")
endif()

# ---- 2. 语言标准 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 3. 选项 ----
option(GATEWAY_BUILD_TESTS "构建单元测试(树莓派/交叉编译可关)" ON)
option(GATEWAY_BUILD_TOOLS "构建开发工具(node-sim 节点模拟器)" ON)

# ---- 4. 编译选项 / sanitizers(定义 gateway_options)----
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerWarnings)

message(STATUS "Project name: ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Tests/Tools : ${GATEWAY_BUILD_TESTS} / ${GATEWAY_BUILD_TOOLS}")

# ---- 5. 共享协议核心,与 STM32 节点同源 ----
# 排在所有网关模块之前,且不依赖 gateway_options:该目录要能被固件工程原样使用,
# 任何对网关构建环境的依赖都会破坏共享前提。详见 protocol/README.md。
add_subdirectory(protocol)

# ---- 6. 网关自身模块(按职责分层,层间依赖见 src/CMakeLists.txt)----
add_subdirectory(src)

# ---- 7. 主程序 ----
# main 只负责 配置 init + 建 GatewayApp + run,具体装配在 app 层。
# gw_app PUBLIC 依赖各层,故这里链接它即可拿到全部传递依赖。
add_executable(gateway src/gateway/app/main.cpp)
target_link_libraries(gateway PRIVATE
    gateway_options
    gw_app
)

# ---- 8. 开发工具 ----
if(GATEWAY_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---- 9. 测试 ----
if(GATEWAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- 10. 安装(树莓派部署:见 README「部署」)----
include(GNUInstallDirs)

# 配置和 unit 是系统级文件，不应跟随 /usr/local 前缀落到
# /usr/local/etc 与 /usr/local/lib/systemd/system。绝对路径仍可通过
# DESTDIR 做打包暂存；实际树莓派部署优先使用 scripts/install_rpi.sh，
# 它会保留已有配置并处理板载 UART。
set(GATEWAY_CONFIG_FILE "/etc/gateway.conf" CACHE FILEPATH
    "网关运行配置的绝对路径")
set(GATEWAY_SYSTEMD_UNIT_DIR "/etc/systemd/system" CACHE PATH
    "gateway.service 的安装目录")

configure_file(
    deploy/gateway.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/gateway.service
    @ONLY
)

get_filename_component(GATEWAY_CONFIG_DIR "${GATEWAY_CONFIG_FILE}" DIRECTORY)
get_filename_component(GATEWAY_CONFIG_NAME "${GATEWAY_CONFIG_FILE}" NAME)
install(TARGETS gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/gateway.conf
        DESTINATION "${GATEWAY_CONFIG_DIR}" RENAME "${GATEWAY_CONFIG_NAME}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gateway.service
        DESTINATION "${GATEWAY_SYSTEMD_UNIT_DIR}")
