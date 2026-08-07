# 测试树:GoogleTest + CTest。
#
# 默认用 FetchContent 拉取源码;树莓派或离线环境可用 -DGATEWAY_USE_SYSTEM_GTEST=ON
# 改用系统 GTest。
#
# 不默认走 find_package 的原因:WSL 上 find_package(GTest) 可能匹配到挂载盘里的
# Windows 版 gtest.lib,配置期正常而链接期报出一批 MSVC 未定义符号。
# 默认自行拉取源码可规避该情形,确有可用系统库时显式开启开关即可。

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
    # gtest 自身的警告不应触发本项目的 -Werror
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# gateway_add_test(<name> SOURCES ... LIBS ... [LABELS ...])
#
# 把建 target、链接 gtest_main、注册进 CTest 三步收拢为一个函数,
# 避免每个测试目录重复同样的样板。
function(gateway_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;LABELS" ${ARGN})

    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE
        ${T_LIBS}
        GTest::gtest_main
        gateway_options        # 测试代码同样遵守零警告基线
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
add_subdirectory(core)
add_subdirectory(io)
add_subdirectory(link)
add_subdirectory(pipeline)
add_subdirectory(cloud)
