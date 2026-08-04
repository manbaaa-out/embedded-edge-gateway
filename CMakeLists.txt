cmake_minimum_required(VERSION 3.21)   # 3.21+:CMakePresets v3
project(gateway VERSION 1.0.0 LANGUAGES C CXX)   # C 用于共享协议核心 edge_proto

# ============================================================
# 顶层只讲「项目由哪些部分组成、它们怎么装在一起」。
# 「每部分怎么编」在 cmake/CompilerWarnings.cmake,
# 「怎么配置构建形态」在 CMakePresets.json。
# ============================================================

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

# ============================================================
# 5. 共享协议核心 —— 与 STM32 节点【同源】的那份代码。
#
# 它排在所有网关模块之前,且刻意不依赖 gateway_options:
# 这份目录要能被固件工程原样拿去用,任何对网关构建环境的依赖
# 都会让「共享」名存实亡。详见 protocol/README.md。
# ============================================================
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
install(TARGETS gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/gateway.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR} RENAME gateway.conf)
install(FILES deploy/gateway.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
