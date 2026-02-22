cmake_minimum_required(VERSION 3.10)
project(benchmark_zrlog LANGUAGES CXX)

# C++17 必需（std::to_chars）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认使用 Release 构建（可通过 -DCMAKE_BUILD_TYPE=Debug 覆盖）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 线程库（用于 std::thread）
find_package(Threads REQUIRED)

# 可执行文件
add_executable(benchmark_zrlog benchmark_zrlog.cpp)

target_link_libraries(benchmark_zrlog PRIVATE Threads::Threads)

# ---------- 编译选项 ----------
# 1. Release 优化标志（显式启用 -O3 或 /O2）
if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
    )
else()
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
endif()

# 2. 警告选项（非强制，但推荐）
if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE /W4)
else()
    target_compile_options(benchmark_zrlog PRIVATE -Wall -Wextra)
endif()