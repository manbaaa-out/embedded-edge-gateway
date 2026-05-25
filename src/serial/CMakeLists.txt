# SerialPort 是有实现的真模块,用 STATIC 库
add_library(serial STATIC
    SerialPort.cpp
)

# 头文件路径暴露给依赖者
target_include_directories(serial PUBLIC .)

# 应用统一编译选项
target_link_libraries(serial PRIVATE
    gateway_options
)
