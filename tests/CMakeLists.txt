# 测试总入口负责获取 GoogleTest、定义统一目标工厂并按模块加入测试目录。
# 默认构建固定版本以保证环境一致；离线或目标机环境可显式使用系统安装版本。
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
    # gtest_force_shared_crt 兼容 Windows CRT；INSTALL_GTEST 防止安装网关时夹带测试依赖。
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# gateway_add_test(name SOURCES ... LIBS ... LABELS ...)：name 是目标名，SOURCES 是测试
# 源文件，LIBS 是被测模块，LABELS 用于 CTest 过滤。函数统一链接测试入口并自动发现 TEST。
function(gateway_add_test name)
    # T_ 前缀保存 cmake_parse_arguments 解析出的多值参数，避免污染调用方变量。
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;LABELS" ${ARGN})

    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE
        ${T_LIBS}
        GTest::gtest_main
        gateway_options        # 测试源码与产品源码使用相同的警告基线。
    )
    # GATEWAY_TEST_DATA_DIR 是绝对源码根，向量测试不依赖 CTest 的工作目录。
    target_compile_definitions(${name} PRIVATE
        GATEWAY_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}"
    )

    # DISCOVERY_TIMEOUT 限制测试枚举阶段，LABELS 原样附加到发现的每个用例。
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
