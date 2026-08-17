# 顶层构建入口负责选择构建形态、装配模块并定义可重定位的安装布局。
cmake_minimum_required(VERSION 3.21)   # CMakePresets.json 使用版本 3 格式。
project(gateway VERSION 1.0.0 LANGUAGES C CXX)   # 网关使用 C++，共享 edge_proto 使用 C。

# 单配置生成器未指定构建类型时采用适合调试的默认值；多配置生成器自行选择配置。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
    message(STATUS "BUILD_TYPE 未指定,默认 Debug")
endif()

# 所有 C++ 目标严格使用标准 C++17，不退回旧标准，也不依赖 GNU 扩展。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 两个开关控制开发期目标，生产或交叉构建可只保留 gateway。
option(GATEWAY_BUILD_TESTS "构建单元测试(树莓派/交叉编译可关)" ON)
option(GATEWAY_BUILD_TOOLS "构建开发工具(node-sim 节点模拟器)" ON)

# gateway_options 集中承载项目代码的警告与 sanitizer 选项。
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerWarnings)

message(STATUS "Project name: ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Tests/Tools : ${GATEWAY_BUILD_TESTS} / ${GATEWAY_BUILD_TOOLS}")

# protocol 定义 edge_proto；它必须保持独立，供网关与 STM32 固件使用同一份实现。
add_subdirectory(protocol)

# src 定义按职责分层的网关静态库，不在此处展开层间依赖。
add_subdirectory(src)

# gateway 是部署产物。main.cpp 只负责进程入口，其余模块通过 gw_app 的 PUBLIC 依赖引入。
add_executable(gateway src/gateway/app/main.cpp)
target_link_libraries(gateway PRIVATE
    gateway_options
    gw_app
)

# tools 包含 node-sim，仅供本地和 CI 联调。
if(GATEWAY_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# 开启 CTest 后由 tests/CMakeLists.txt 将每个 GoogleTest 用例自动发现为独立条目。
if(GATEWAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# GNUInstallDirs 提供 bindir/sysconfdir 等平台惯例目录变量。
include(GNUInstallDirs)

# GATEWAY_CONFIG_FILE 写入 systemd unit，必须是目标机上的绝对运行路径；默认值与
# install_rpi.sh 的生产布局一致。`cmake --install --prefix` 只改安装目的地，不会重写
# unit 内路径，因此改变安装前缀的打包方也应在配置阶段同步覆盖此变量。
set(GATEWAY_CONFIG_FILE "/etc/gateway.conf" CACHE FILEPATH
    "网关运行配置的绝对路径")

# 仅替换 @VAR@ 占位符，避免 systemd 的 `$MAINPID` 被 CMake 当作变量处理。
configure_file(
    deploy/gateway.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/gateway.service
    @ONLY
)

# 分别安装可执行文件、默认配置和配置阶段生成的 unit；DESTINATION 保持相对路径。
install(TARGETS gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/gateway.conf
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR} RENAME gateway.conf)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gateway.service
        DESTINATION lib/systemd/system)
