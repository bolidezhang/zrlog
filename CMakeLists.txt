cmake_minimum_required(VERSION 3.10)
project(benchmark_zrlog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(benchmark_zrlog benchmark_zrlog.cpp)
add_executable(benchmark_zrlog_v2 benchmark_zrlog_v2.cpp)

target_link_libraries(benchmark_zrlog PRIVATE Threads::Threads)
target_link_libraries(benchmark_zrlog_v2 PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
    )
    target_compile_options(benchmark_zrlog_v2 PRIVATE
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:RelWithDebInfo>:/O2>
    )
else()
    target_compile_options(benchmark_zrlog PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
    target_compile_options(benchmark_zrlog_v2 PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
endif()

if(MSVC)
    target_compile_options(benchmark_zrlog PRIVATE /W4)
    target_compile_options(benchmark_zrlog_v2 PRIVATE /W4)
else()
    target_compile_options(benchmark_zrlog PRIVATE -Wall -Wextra)
    target_compile_options(benchmark_zrlog_v2 PRIVATE -Wall -Wextra)
endif()
