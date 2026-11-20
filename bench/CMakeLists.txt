# Cross-library benchmark: zrlog vs spdlog / fmtlog / NanoLog
#
# Each library builds into its own executable because they vendor different
# fmt versions and cannot share a binary. The shared measurement code lives in
# bench_common.h.
#
# Third-party sources are fetched at configure time via FetchContent into
# bench/_deps, i.e. inside this project rather than under the build tree, so
# deleting and recreating build-bench/ does not force a re-download. That
# directory is gitignored.
#
# To use pre-cloned sources instead:
#   -DZRLOG_BENCH_FETCH_DEPS=OFF -DZRLOG_BENCH_DEPS=/path/to/deps
# or point CMake's standard override at each one:
#   -DFETCHCONTENT_SOURCE_DIR_SPDLOG=/path/to/spdlog
#   -DFETCHCONTENT_SOURCE_DIR_FMTLOG=/path/to/fmtlog
#   -DFETCHCONTENT_SOURCE_DIR_NANOLOG=/path/to/NanoLog

include(FetchContent)

# FETCHCONTENT_BASE_DIR is set by the top-level CMakeLists (bench/_deps) before
# add_subdirectory(bench) - it has to be set there, not here, because the
# sub-build cache is created at the top level.

option(ZRLOG_BENCH_FETCH_DEPS "Fetch spdlog/fmtlog/quill/NanoLog at configure time" ON)
set(ZRLOG_BENCH_DEPS "" CACHE PATH
    "Pre-cloned dependency root (spdlog/, fmtlog/, nanolog/); empty = use FetchContent")
set(ZRLOG_BENCH_GIT_MIRROR "" CACHE STRING
    "Optional git mirror prefix for GitHub, e.g. https://gh-proxy.com/ .
     Useful where direct GitHub access resets. Empty = clone from GitHub directly.")

# Prefix a GitHub URL with the mirror when one is configured.
function(zrlog_bench_git_url out_var url)
    if(ZRLOG_BENCH_GIT_MIRROR)
        set(${out_var} "${ZRLOG_BENCH_GIT_MIRROR}${url}" PARENT_SCOPE)
    else()
        set(${out_var} "${url}" PARENT_SCOPE)
    endif()
endfunction()

# Resolve a dependency directory: explicit ZRLOG_BENCH_DEPS wins, else the
# FetchContent-populated location.
function(zrlog_bench_dep_dir out_var name fetch_subdir local_subdir)
    if(ZRLOG_BENCH_DEPS AND EXISTS "${ZRLOG_BENCH_DEPS}/${local_subdir}")
        set(${out_var} "${ZRLOG_BENCH_DEPS}/${local_subdir}" PARENT_SCOPE)
    else()
        set(${out_var} "${FETCHCONTENT_BASE_DIR}/${name}-src/${fetch_subdir}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# spdlog (header-only target)
# ---------------------------------------------------------------------------
if(ZRLOG_BENCH_FETCH_DEPS AND NOT ZRLOG_BENCH_DEPS)
    zrlog_bench_git_url(SPDLOG_URL https://github.com/gabime/spdlog.git)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY ${SPDLOG_URL}
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    # Populate only: the benchmark needs headers, not spdlog's full build
    # (examples/tests would slow configure down considerably).
    FetchContent_Populate(spdlog)
endif()
zrlog_bench_dep_dir(SPDLOG_INCLUDE spdlog "include" "spdlog/include")

# ---------------------------------------------------------------------------
# fmtlog + fmt
# fmtlog's own fmt submodule is empty upstream, so reuse the fmt bundled with
# zrlog (v12 is sufficient: fmtlog only needs format_string/make_format_args).
# FMT_HEADER_ONLY is required for report_error() to resolve.
# ---------------------------------------------------------------------------
if(ZRLOG_BENCH_FETCH_DEPS AND NOT ZRLOG_BENCH_DEPS)
    zrlog_bench_git_url(FMTLOG_URL https://github.com/MengRao/fmtlog.git)
    FetchContent_Declare(fmtlog
        GIT_REPOSITORY ${FMTLOG_URL}
        GIT_TAG        main          # upstream default branch is "main", not master
        GIT_SHALLOW    TRUE
        # fmtlog declares a "fmt" submodule, but we reuse zrlog's fmt (its own
        # submodule is empty upstream anyway). Skipping avoids a second clone
        # that would fail where GitHub submodule URLs are unreachable.
        GIT_SUBMODULES  ""
        GIT_SUBMODULES_RECURSE FALSE
    )
    # Populate only: fmtlog's own CMakeLists does add_subdirectory(fmt), which
    # would fail since its fmt submodule is empty. We compile fmtlog.cc directly.
    FetchContent_Populate(fmtlog)
endif()
zrlog_bench_dep_dir(FMTLOG_DIR fmtlog "" "fmtlog")

# ---------------------------------------------------------------------------
# quill (header-only, cross-platform)
# ---------------------------------------------------------------------------
if(ZRLOG_BENCH_FETCH_DEPS AND NOT ZRLOG_BENCH_DEPS)
    zrlog_bench_git_url(QUILL_URL https://github.com/odygrd/quill.git)
    FetchContent_Declare(quill
        GIT_REPOSITORY ${QUILL_URL}
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    # Populate only: the benchmark needs headers, not quill's own build/tests.
    FetchContent_Populate(quill)
endif()
zrlog_bench_dep_dir(QUILL_INCLUDE quill "include" "quill/include")

# ---------------------------------------------------------------------------
# NanoLog (Linux only - depends on POSIX AIO)
# ---------------------------------------------------------------------------
if(NOT WIN32)
    if(ZRLOG_BENCH_FETCH_DEPS AND NOT ZRLOG_BENCH_DEPS)
        zrlog_bench_git_url(NANOLOG_URL https://github.com/PlatformLab/NanoLog.git)
        FetchContent_Declare(nanolog
            GIT_REPOSITORY ${NANOLOG_URL}
            GIT_TAG        master
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(nanolog)
    endif()
    zrlog_bench_dep_dir(NANOLOG_RUNTIME nanolog "runtime" "nanolog/runtime")
endif()

# ===========================================================================
# Targets
# ===========================================================================
function(zrlog_bench_target name source)
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(MSVC)
        # /utf-8: fmt 12 static-asserts without it; NOMINMAX keeps windows.h sane
        target_compile_options(${name} PRIVATE /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS)
        target_compile_options(${name} PRIVATE $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(${name} PRIVATE -O3 -march=native)
    endif()
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endfunction()

# zrlog
zrlog_bench_target(bench_zrlog bench_zrlog.cpp)
target_link_libraries(bench_zrlog PRIVATE zrlog)

# spdlog
if(EXISTS "${SPDLOG_INCLUDE}/spdlog/spdlog.h")
    zrlog_bench_target(bench_spdlog bench_spdlog.cpp)
    target_include_directories(bench_spdlog PRIVATE "${SPDLOG_INCLUDE}")
else()
    message(STATUS "bench: spdlog not found, skipping bench_spdlog")
endif()

# fmtlog
if(EXISTS "${FMTLOG_DIR}/fmtlog.cc")
    zrlog_bench_target(bench_fmtlog bench_fmtlog.cpp "${FMTLOG_DIR}/fmtlog.cc")
    target_include_directories(bench_fmtlog PRIVATE "${FMTLOG_DIR}" "${CMAKE_SOURCE_DIR}/fmt/include")
    target_compile_definitions(bench_fmtlog PRIVATE FMT_HEADER_ONLY)
    # 队列容量对齐 bench_zrlog 的 8MB 线程缓冲。必须同时作用于 fmtlog.cc 与
    # bench_fmtlog.cpp：ThreadBuffer 在 fmtlog.cc 里 new、在 bench TU 里内联
    # alloc，定义不一致会 ODR 冲突并堆越界。target_compile_definitions 天然
    # 对同一 target 的所有源文件生效。
    target_compile_definitions(bench_fmtlog PRIVATE FMTLOG_BENCH_QUEUE_MB=8)
else()
    message(STATUS "bench: fmtlog not found, skipping bench_fmtlog")
endif()

# quill
if(EXISTS "${QUILL_INCLUDE}/quill/Backend.h")
    zrlog_bench_target(bench_quill bench_quill.cpp)
    target_include_directories(bench_quill PRIVATE "${QUILL_INCLUDE}")
else()
    message(STATUS "bench: quill not found, skipping bench_quill")
endif()

# NanoLog
if(WIN32)
    message(STATUS "bench: NanoLog is Linux-only, skipping bench_nanolog")
elseif(EXISTS "${NANOLOG_RUNTIME}/libNanoLog.a")
    zrlog_bench_target(bench_nanolog bench_nanolog.cpp)
    target_include_directories(bench_nanolog PRIVATE "${NANOLOG_RUNTIME}")
    target_compile_options(bench_nanolog PRIVATE -Werror=format)
    target_link_libraries(bench_nanolog PRIVATE "${NANOLOG_RUNTIME}/libNanoLog.a" rt)
else()
    message(STATUS "bench: NanoLog runtime not built; run 'make libNanoLog.a' in "
                   "${NANOLOG_RUNTIME} then re-configure")
endif()
