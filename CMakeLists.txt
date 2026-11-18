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
# 跨库对比基准（spdlog/fmtlog/NanoLog）。默认关闭：配置时需要联网克隆
# 第三方仓库，不适合作为默认构建的一部分。
option(ZRLOG_BUILD_CROSS_BENCH "构建跨库对比基准 bench/（需联网下载第三方库）" OFF)

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
# 跨库对比基准（zrlog vs spdlog / fmtlog / NanoLog）
# ============================================================================
if(ZRLOG_BUILD_CROSS_BENCH)
    enable_testing()   # FetchContent 内部可能用到
    add_subdirectory(bench)
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

    # 异常屏障：自定义 formatter 抛异常时后端线程不得崩溃（进程须存活并输出 [FMT_ERROR]）
    add_executable(zrlog_exception_test tests/exception_test.cpp)
    target_link_libraries(zrlog_exception_test PRIVATE zrlog)
    if(MSVC)
        target_compile_options(zrlog_exception_test PRIVATE /W4)
    else()
        target_compile_options(zrlog_exception_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME zrlog_exception_barrier
             COMMAND zrlog_exception_test
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(zrlog_exception_barrier PROPERTIES TIMEOUT 60)

    # 毒丸消息：decoder 抛任意异常时不得堵塞后台线程（曾导致 fini() 永久挂起）
    add_executable(zrlog_poison_test tests/poison_test.cpp)
    target_link_libraries(zrlog_poison_test PRIVATE zrlog)
    if(MSVC)
        target_compile_options(zrlog_poison_test PRIVATE /W4)
    else()
        target_compile_options(zrlog_poison_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME zrlog_poison_pill
             COMMAND zrlog_poison_test
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(zrlog_poison_pill PROPERTIES TIMEOUT 60)

    # 崩溃抢救：子进程触发 SIGSEGV，父进程校验崩溃前日志已落盘
    # （仅 Windows：使用 CreateProcess 隔离崩溃，避免父进程被信号杀死）
    if(WIN32)
        add_executable(zrlog_crash_test tests/crash_test.cpp)
        target_link_libraries(zrlog_crash_test PRIVATE zrlog)
        if(MSVC)
            target_compile_options(zrlog_crash_test PRIVATE /W4)
        else()
            target_compile_options(zrlog_crash_test PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME zrlog_crash_flush
                 COMMAND zrlog_crash_test
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(zrlog_crash_flush PROPERTIES TIMEOUT 60)
    endif()
endif()
