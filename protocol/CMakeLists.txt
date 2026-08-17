# 网关与 STM32 共用的 C99 协议实现。该目标保持自包含，不依赖网关侧 target 或第三方库。
# 固件既可通过 add_subdirectory() 链接 edge_proto，也可直接编译 src/ 并包含 include/。

cmake_minimum_required(VERSION 3.15)

# 当本目录不是由网关工程 add_subdirectory() 引入时，声明最小独立 C 项目；
# PROJECT_NAME 已存在时不得覆盖父项目名称、版本或语言集合。
if(NOT DEFINED PROJECT_NAME)
    project(edge_proto VERSION 1.2.0 LANGUAGES C)
endif()

# edge_proto 包含双方必须逐字节一致的 CRC 与帧状态机实现。
add_library(edge_proto STATIC
    src/edge_crc16.c
    src/edge_frame.c
)

# PUBLIC include 路径随静态库传给网关、测试和固件侧消费者。
target_include_directories(edge_proto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# C99 和禁用编译器扩展是固件工具链兼容性要求；PIC 允许同一静态库进入共享产物。
set_target_properties(edge_proto PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# 告警在本 target 内声明，确保独立构建不依赖父工程的 gateway_options。
target_compile_options(edge_proto PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
)

# 此处有意不声明链接依赖，确保协议核心可直接编入固件。
