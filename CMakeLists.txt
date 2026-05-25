cmake_minimum_required(VERSION 3.15)
project(gateway VERSION 0.1.0 LANGUAGES CXX)

# 1. Build type 防御
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
    message(STATUS "BUILD_TYPE 未指定,默认 Debug")
endif()

# 2. C++ 标准三件套
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 3. 用 INTERFACE 库收纳全项目编译选项(modern CMake 推荐)
add_library(gateway_options INTERFACE)
target_compile_options(gateway_options INTERFACE
    -Wall -Wextra -Wpedantic
)

# 4. 打印配置信息(方便排查环境问题)
message(STATUS "Project name : ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler     : ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ standard : ${CMAKE_CXX_STANDARD}")

# 5. 引入子模块
add_subdirectory(src/log)
add_subdirectory(src/serial)

# 6. 主程序
add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE
    gateway_options       # 编译选项
    log                   # 日志模块
    serial                # 串口模块
)
