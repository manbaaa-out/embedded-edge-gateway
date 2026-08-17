# gateway_options 是无产物的策略目标。只有项目自有目标显式链接它，既保证统一
# 警告和 sanitizer 参数，又不会把 -Werror 传染给 FetchContent 的第三方源码。
add_library(gateway_options INTERFACE)
target_compile_options(gateway_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow                # 防止局部变量意外遮蔽成员或外层变量。
    -Wnon-virtual-dtor      # 检查多态基类缺少虚析构函数。
    -Wcast-align            # 检查在 ARM 上可能导致未对齐访问的转换。
    -Wunused                # 检查未使用的变量、参数和函数。
    -Woverloaded-virtual    # 防止派生类意外隐藏基类虚函数重载。
    -Wconversion            # 检查协议字段转换中的隐式窄化。
    -Wsign-conversion       # 检查有符号与无符号协议长度之间的隐式转换。
    -Wdouble-promotion      # 检查 float 在格式化和计算中被隐式提升。
    -Wformat=2              # 对 printf 类调用执行更严格的格式串检查。
)

# CI/dev 可将所有上述警告提升为错误；目标机临时诊断构建可关闭。
option(GATEWAY_WARNINGS_AS_ERRORS "把警告当错误(CI 守零警告基线)" OFF)
if(GATEWAY_WARNINGS_AS_ERRORS)
    target_compile_options(gateway_options INTERFACE -Werror)
endif()

# GATEWAY_ENABLE_ASAN 同时启用 AddressSanitizer 和 UndefinedBehaviorSanitizer；
# GATEWAY_ENABLE_TSAN 单独检查数据竞争。两套运行时不能链接进同一产物。
option(GATEWAY_ENABLE_ASAN "启用 AddressSanitizer + UBSan" OFF)
option(GATEWAY_ENABLE_TSAN "启用 ThreadSanitizer" OFF)

if(GATEWAY_ENABLE_ASAN AND GATEWAY_ENABLE_TSAN)
    message(FATAL_ERROR "ASan 与 TSan 不能同时启用,请分别用 asan / tsan preset")
endif()

if(GATEWAY_ENABLE_ASAN)
    # 编译和链接阶段都必须注入 sanitizer；保留帧指针提高错误栈可读性。
    target_compile_options(gateway_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(gateway_options INTERFACE -fsanitize=address,undefined)
    message(STATUS "Sanitizer   : ASan + UBSan")
endif()

if(GATEWAY_ENABLE_TSAN)
    # TSan 使用独立编译/链接参数，通常由 tsan preset 开启。
    target_compile_options(gateway_options INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(gateway_options INTERFACE -fsanitize=thread)
    message(STATUS "Sanitizer   : TSan")
endif()
