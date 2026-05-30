# Log module: Logger 同步外壳 + AsyncLogger 异步后端
add_library(log STATIC
    Logger.cpp
    AsyncLogger.cpp
)
target_include_directories(log PUBLIC .)

# AsyncLogger 用 std::thread, 需要 pthread
# PUBLIC 因为 Logger.h 间接引入 AsyncLogger.h, 下游可执行文件也间接依赖
find_package(Threads REQUIRED)
target_link_libraries(log
    PRIVATE gateway_options
    PUBLIC Threads::Threads
)
