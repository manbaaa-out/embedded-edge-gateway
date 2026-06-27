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
add_subdirectory(src/protocol)
add_subdirectory(src/concurrent)
add_subdirectory(src/net)
add_subdirectory(src/db)
add_subdirectory(src/mqtt)
add_subdirectory(src/config) 

add_subdirectory(src/app)     # GatewayApp:网关应用对象(资源+装配+主循环)

# 6. 主程序:main 只负责 配置 init + 建 GatewayApp + run,具体装配在 app 模块。
#    app PUBLIC 依赖各 lib,故这里链接 app 即可拿到全部传递依赖。
add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE
    gateway_options       # 编译选项
    app                   # 网关应用对象(传递依赖 log/serial/protocol/concurrent/db/mqtt/net/config)
)
