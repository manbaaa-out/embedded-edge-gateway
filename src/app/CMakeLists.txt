# App 模块:GatewayApp —— 网关应用对象(资源 + 装配 + 主循环)
# 由原 main.cpp 抽取而来,消除工厂函数的多引用参数传递。
add_library(app STATIC
    GatewayApp.cpp
)

# 头文件路径暴露给依赖者(main.cpp include "GatewayApp.h")
target_include_directories(app PUBLIC .)

# GatewayApp.h 头里就用到这些类型(Database/MqttClient/SerialPort/ThreadPool/
# EventLoop/FrameParser/ThreadSafeQueue),故对它们 PUBLIC,使 main 也拿到 include;
# gateway_options 只是本模块编译选项,PRIVATE 即可。
target_link_libraries(app
    PUBLIC  log serial protocol concurrent db mqtt net config
    PRIVATE gateway_options
)
