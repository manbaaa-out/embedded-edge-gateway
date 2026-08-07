# app 层:资源持有、装配与主循环。
#
# 这是唯一允许同时可见所有下层的 target。任何下层 target 都不得链接 gw_app,
# 依赖方向唯一。
add_library(gw_app STATIC
    GatewayApp.cpp
)

# GatewayApp.h 的成员类型来自这些层,故 PUBLIC(main.cpp 也要看见它们);
# gateway_options 只是本模块的编译选项,PRIVATE 即可。
target_link_libraries(gw_app
    PUBLIC  gateway_includes
            gw_log gw_config gw_concurrent
            gw_serial gw_event gw_http
            gw_protocol gw_storage gw_cloud
            gw_link gw_pipeline
    PRIVATE gateway_options
)
