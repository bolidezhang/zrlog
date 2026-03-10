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

# ===== 修改：fmt 仅作为头文件库使用，只需添加包含路径 =====
# 假设头文件位于 ./fmt 目录下（例如 ./fmt/fmt/core.h）
# 如果 fmt 目录内部还有 include 子目录，请相应调整
target_include_directories(benchmark_zrlog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fmt/include)

# 可执行文件
add_executable(benchmark_zrlog benchmark_zrlog.cpp)

# 链接库：仅需要 Threads（fmt 无需链接）
target_link_libraries(benchmark_zrlog PRIVATE
    Threads::Threads
)

# ---------- 编译选项 ----------
# 1. Release 优化标志（显式启用 -O3 或 /O2）
if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
        # MSVC 高级优化选项
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/GL>  # 全程序优化
    )
    # MSVC 链接时优化
    target_link_options(benchmark_zrlog PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/LTCG> # 链接时代码生成
    )
else()
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
        # GCC/Clang 常见优化选项
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-march=native>  # 针对本地架构优化
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto>          # 链接时优化（同时作用于编译和链接）
    )
    # GCC/Clang 链接时优化（-flto 也需要在链接阶段启用）
    target_link_options(benchmark_zrlog PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto>
    )
endif()

# 2. 警告选项（非强制，但推荐）
if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE /W4)
else()
    target_compile_options(benchmark_zrlog PRIVATE -Wall -Wextra)
endif()