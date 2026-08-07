# edge_proto —— 上下位机共享的协议核心
#
# 本文件刻意自包含:不引用父项目的任何 target(gateway_options 等)、不 find_package、
# 不设编译器路径。这份代码要能被 STM32 固件工程原样拿去编译,任何对网关侧构建环境
# 的依赖都会让「共享」名存实亡。
#
# 消费方式(STM32 侧用后者):
#   1. add_subdirectory(protocol) → link edge_proto
#   2. 直接把 src/*.c 加进自己的 target,include/ 加进 include 路径

cmake_minimum_required(VERSION 3.15)

# 允许独立构建,便于单独跑一致性测试而不必配置整个网关
if(NOT DEFINED PROJECT_NAME)
    project(edge_proto VERSION 1.2.0 LANGUAGES C)
endif()

add_library(edge_proto STATIC
    src/edge_crc16.c
    src/edge_frame.c
)

target_include_directories(edge_proto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# C99 是契约的一部分:MCU 工具链未必提供 C11/C17
set_target_properties(edge_proto PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF          # 不用 GNU 扩展,以便更换编译器
    POSITION_INDEPENDENT_CODE ON
)

# 本层自带警告选项,不从父项目继承(见文件头)
target_compile_options(edge_proto PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
)

# 协议核心必须自足:不链接任何库,连 libm 也不链。
# CI 中有一个 job 用 arm-none-eabi-gcc 编译本目录并检查未定义符号,
# 任何一个 #include <stdio.h> 或 malloc 调用都会在那里暴露。
