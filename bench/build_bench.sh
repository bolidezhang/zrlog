#!/usr/bin/env bash
# 编译跨库对比基准（Linux/macOS）。
#
# 第三方库不入库，需要提供：
#   $ZRLOG_BENCH_DEPS/spdlog/include/   (gabime/spdlog v1.x)
#   $ZRLOG_BENCH_DEPS/fmtlog/           (MengRao/fmtlog，含 fmtlog.cc)
#   $ZRLOG_BENCH_DEPS/nanolog/runtime/  (PlatformLab/NanoLog，需先 make libNanoLog.a)
#
# 用法: ./build_bench.sh
# 产物: bench/bin/bench_<lib>（与 Windows 脚本命名一致）
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DEPS="${ZRLOG_BENCH_DEPS:-$ROOT/../zrlog-bench}"
OUT="$HERE/bin"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O3 -march=native -DNDEBUG -pthread"

mkdir -p "$OUT"

build_one() {
    local name="$1"; shift
    echo "[$name]"
    "$CXX" $FLAGS "$@" -o "$OUT/bench_$name" || { echo "FAILED: $name"; exit 1; }
}

build_one zrlog  -I"$ROOT" -I"$ROOT/fmt/include" "$HERE/bench_zrlog.cpp"

if [ -d "$DEPS/spdlog/include" ]; then
    build_one spdlog -I"$HERE" -I"$DEPS/spdlog/include" "$HERE/bench_spdlog.cpp"
else
    echo "[spdlog] skipped (no $DEPS/spdlog)"
fi

if [ -d "$DEPS/fmtlog" ]; then
    build_one fmtlog -DFMT_HEADER_ONLY -I"$HERE" -I"$DEPS/fmtlog" \
        -I"$ROOT/fmt/include" "$HERE/bench_fmtlog.cpp" "$DEPS/fmtlog/fmtlog.cc"
else
    echo "[fmtlog] skipped (no $DEPS/fmtlog)"
fi

if [ -d "$DEPS/quill/include" ]; then
    build_one quill -I"$HERE" -I"$DEPS/quill/include" "$HERE/bench_quill.cpp"
else
    echo "[quill] skipped (no $DEPS/quill)"
fi

if [ -f "$DEPS/nanolog/runtime/libNanoLog.a" ]; then
    build_one nanolog -I"$HERE" -I"$DEPS/nanolog/runtime" -Werror=format \
        "$HERE/bench_nanolog.cpp" \
        -L"$DEPS/nanolog/runtime" -lNanoLog -lrt
else
    echo "[nanolog] skipped (run 'make libNanoLog.a' in NanoLog/runtime first)"
fi

echo "ALL_DONE  ->  $OUT"
