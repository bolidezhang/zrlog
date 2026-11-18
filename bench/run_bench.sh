#!/usr/bin/env bash
# 多轮轮转基准：每库每轮一个独立进程，顺序每轮轮转，最后取中位数。
# 用法: ./run_bench.sh [rounds] [threads] [per_thread]
set -u
ROUNDS=${1:-5}
THREADS=${2:-4}
PER=${3:-2000000}
LIBS=(zrlog spdlog fmtlog nanolog)
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$BIN_DIR/results"
mkdir -p "$OUT"
rm -f "$OUT"/*.txt

# 固定 CPU 频率/调度尽量稳定（无权限时静默跳过）
echo "== env ==" > "$OUT/env.txt"
lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|MHz' >> "$OUT/env.txt" 2>&1
g++ --version | head -1 >> "$OUT/env.txt"
uname -r >> "$OUT/env.txt"
cat "$OUT/env.txt"

for ((r=0; r<ROUNDS; ++r)); do
  # 每轮把库顺序整体左移一位，让冷缓存/热缓存偏置均摊
  rotated=()
  for ((i=0; i<${#LIBS[@]}; ++i)); do
    rotated+=("${LIBS[$(((i + r) % ${#LIBS[@]}))]}")
  done
  echo "== round $r order: ${rotated[*]} =="
  for lib in "${rotated[@]}"; do
    timeout 300 "$BIN_DIR/bin_$lib" "$THREADS" "$PER" both \
      | grep -E 'LATENCY|THROUGHPUT|^  ' >> "$OUT/$lib.txt"
    echo "  $lib done"
  done
done
echo "== raw results in $OUT =="
