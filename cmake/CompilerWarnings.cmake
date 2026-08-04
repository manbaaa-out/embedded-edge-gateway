# ============================================================
# 全项目统一编译选项(modern CMake:用 INTERFACE 库收纳)
#
# 从顶层 CMakeLists 移出来单独成文件的理由:顶层只该讲「项目由哪些部分组成」,
# 「每个部分怎么编」是另一件事。两者混在一起,顶层文件会随选项增多而失焦。
#
# 与 STM32 节点侧 cmake/gcc-arm-none-eabi.cmake + n6_options 同构:
#   基线警告全局开,-Werror 只给自己写的代码,不传染给第三方(gtest 等)。
# ============================================================

# ---- gateway_options:自己写的 C++ 代码用这一份 ----
add_library(gateway_options INTERFACE)
target_compile_options(gateway_options INTERFACE
    -Wall -Wextra -Wpedantic
    # 下面几条是在基线之上补的「工业级」项,均已验证本仓库 0 warning:
    -Wshadow                # 局部变量遮蔽成员/外层变量(热加载那类代码的高发 bug)
    -Wnon-virtual-dtor      # 有虚函数却无虚析构(Sink 这类接口类的经典坑)
    -Wcast-align            # 可能引发未对齐访问的指针转换(ARM 上会真的崩)
    -Wunused
    -Woverloaded-virtual
    -Wconversion            # 隐式窄化:协议代码里 int→uint8_t 全靠它盯着
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
)

# ---- 零警告基线:CI 与 dev preset 打开,交叉编译等场景可关 ----
option(GATEWAY_WARNINGS_AS_ERRORS "把警告当错误(CI 守零警告基线)" OFF)
if(GATEWAY_WARNINGS_AS_ERRORS)
    target_compile_options(gateway_options INTERFACE -Werror)
endif()

# ============================================================
# Sanitizers:通过 preset 打开,不改代码即可换构建形态
#   ASan  查 use-after-free —— 正是热加载 reset db_/client_ 时最怕的那类 bug
#   UBSan 查未定义行为 —— 协议解析里的移位/窄化重灾区
#   TSan  与 ASan 互斥,单独 preset
# ============================================================
option(GATEWAY_ENABLE_ASAN "启用 AddressSanitizer + UBSan" OFF)
option(GATEWAY_ENABLE_TSAN "启用 ThreadSanitizer" OFF)

if(GATEWAY_ENABLE_ASAN AND GATEWAY_ENABLE_TSAN)
    message(FATAL_ERROR "ASan 与 TSan 不能同时启用,请分别用 asan / tsan preset")
endif()

if(GATEWAY_ENABLE_ASAN)
    target_compile_options(gateway_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(gateway_options INTERFACE -fsanitize=address,undefined)
    message(STATUS "Sanitizer   : ASan + UBSan")
endif()

if(GATEWAY_ENABLE_TSAN)
    target_compile_options(gateway_options INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(gateway_options INTERFACE -fsanitize=thread)
    message(STATUS "Sanitizer   : TSan")
endif()
