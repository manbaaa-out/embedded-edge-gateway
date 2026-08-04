# ============================================================
# 测试树:GoogleTest + CTest
#
# 依赖获取策略:默认 FetchContent 拉源码,可用 -DGATEWAY_USE_SYSTEM_GTEST=ON
# 改用系统 GTest(树莓派 / 离线机器 apt install libgtest-dev 后更快)。
#
# 【为什么不默认 find_package】WSL 上 find_package(GTest) 会捞到挂载盘里的
# Windows 版 gtest.lib(如 miniconda 带的那份),配置期一切正常,链接期才炸出
# 一串 MSVC 符号未定义。默认自己拉源码,把这种「环境里躺着一个不兼容同名库」
# 的坑挡在门外;真有可用系统库的人显式开开关即可。
# ============================================================

option(GATEWAY_USE_SYSTEM_GTEST "用系统安装的 GTest 而非 FetchContent 拉源码" OFF)

if(GATEWAY_USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
    message(STATUS "Tests       : 使用系统 GTest")
else()
    message(STATUS "Tests       : FetchContent 拉取 GTest v1.15.2")
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # gtest 自身的警告不该卡住我们的 -Werror(它不是我们写的代码)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# ------------------------------------------------------------
# gateway_add_test(<name> SOURCES ... LIBS ... [LABELS ...])
#
# 把「建 target + 链 gtest_main + 注册进 CTest」这三步收成一个函数,
# 免得每个测试文件都抄一遍五行样板。
# ------------------------------------------------------------
function(gateway_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;LABELS" ${ARGN})

    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE
        ${T_LIBS}
        GTest::gtest_main
        gateway_options        # 测试代码同样守零警告基线
    )
    # 测试里要读 protocol/vectors/frames.csv 这类数据文件,给个绝对路径宏,
    # 免得测试可执行文件的工作目录一变就找不到数据。
    target_compile_definitions(${name} PRIVATE
        GATEWAY_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}"
    )

    gtest_discover_tests(${name}
        PROPERTIES LABELS "${T_LABELS}"
        DISCOVERY_TIMEOUT 30
    )
endfunction()

add_subdirectory(protocol)
