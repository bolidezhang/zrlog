cmake_minimum_required(VERSION 3.16)
project(zrlog LANGUAGES CXX)

# zrlog 是单头文件库（zrlog.h），C++17 起可用
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认构建类型（多配置生成器如 VS 会忽略此项）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ZRLOG_BUILD_BENCHMARK "构建性能基准 benchmark_zrlog" ON)
option(ZRLOG_BUILD_TESTS "构建单元测试" ON)

find_package(Threads REQUIRED)

# ============================================================================
# 头文件库目标：使用者只需 target_link_libraries(app PRIVATE zrlog)
# ============================================================================
add_library(zrlog INTERFACE)
target_include_directories(zrlog INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}          # zrlog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fmt/include  # fmtlib (header-only)
)
target_compile_features(zrlog INTERFACE cxx_std_17)
target_link_libraries(zrlog INTERFACE Threads::Threads)

# MSVC：fmt 12.x 的 Unicode 支持要求 /utf-8，否则静态断言失败
if(MSVC)
    target_compile_options(zrlog INTERFACE /utf-8)
endif()

# ============================================================================
# 公共编译选项（仅本仓库自建的 benchmark/tests 使用，不污染使用者工程）
# ============================================================================
function(zrlog_apply_build_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/GL>
        )
        target_link_options(${target} PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/LTCG>
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O3>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-march=native>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto>
        )
        target_link_options(${target} PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto>
        )
    endif()
endfunction()

# ============================================================================
# 性能基准
# ============================================================================
if(ZRLOG_BUILD_BENCHMARK)
    add_executable(benchmark_zrlog benchmark_zrlog.cpp)
    target_link_libraries(benchmark_zrlog PRIVATE zrlog)
    zrlog_apply_build_flags(benchmark_zrlog)
endif()

# ============================================================================
# 单元测试（ctest）
# ============================================================================
if(ZRLOG_BUILD_TESTS)
    enable_testing()

    add_executable(zrlog_tests tests/zrlog_tests.cpp)
    target_link_libraries(zrlog_tests PRIVATE zrlog)
    # 测试追求可调试与快速反馈，不做 -O3/-flto 等激进优化
    if(MSVC)
        target_compile_options(zrlog_tests PRIVATE /W4)
    else()
        target_compile_options(zrlog_tests PRIVATE -Wall -Wextra)
    endif()

    # 测试会写日志文件，统一放在构建目录下，避免污染源码树
    add_test(NAME zrlog_unit_tests
             COMMAND zrlog_tests
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(zrlog_unit_tests PROPERTIES TIMEOUT 120)
endif()
