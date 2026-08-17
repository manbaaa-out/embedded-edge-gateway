# 文件职责：定义应用组合层。该层可以依赖所有下层模块，下层模块不得反向链接 gw_app，
# 以保持“基础设施/业务组件 -> 应用装配 -> 可执行文件”的单向依赖。

# gw_app 是只包含 GatewayApp 实现的静态库；进程入口由 src/CMakeLists.txt 单独编译，
# 这样测试或其他入口可以复用装配逻辑而不引入第二个 main()。
add_library(gw_app STATIC
    GatewayApp.cpp
)

# GatewayApp.h 在成员和方法签名中直接使用这些类型，所以依赖采用 PUBLIC，继续传递给
# 包含该头文件的目标。gateway_options 和 gw_format 只影响 GatewayApp.cpp，保持 PRIVATE。
target_link_libraries(gw_app
    PUBLIC
        gateway_includes                 # 项目统一 include 根目录
        gw_log gw_config gw_concurrent   # 日志、热加载配置和跨线程命令队列
        gw_serial gw_event gw_http       # fd 封装、Reactor 与监控服务
        gw_protocol gw_storage gw_cloud  # 帧协议、SQLite 与 MQTT 命令入口
        gw_link gw_pipeline              # STM32 链路、重试跟踪和遥测落库
    PRIVATE
        gateway_options                  # 本目标统一的编译告警和语言选项
        gw_format                        # ACK/查询响应的数值序列化
)
