# 本文件定义源码层次的装配顺序。gateway_includes 是纯 include-path 目标，各模块
# 通过链接它共享 src/ 根目录，因此公共头统一以 gateway/ 开头引用。
add_library(gateway_includes INTERFACE)
target_include_directories(gateway_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# 子目录按“被依赖者先定义”排列：core/protocol 是基础层，storage/cloud/io 提供
# 外部适配，link/pipeline 实现业务流，app 最终看到并装配全部下层。
add_subdirectory(gateway/core)
add_subdirectory(gateway/protocol)
add_subdirectory(gateway/storage)
add_subdirectory(gateway/cloud)
add_subdirectory(gateway/io)
add_subdirectory(gateway/link)
add_subdirectory(gateway/pipeline)
add_subdirectory(gateway/app)
