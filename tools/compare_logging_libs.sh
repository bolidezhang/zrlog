#!/usr/bin/env bash
set -euo pipefail

# Unified benchmark harness (local machine) for:
# zrlog / spdlog / quill / fmtlog / nanolog
#
# NOTE:
# - This script only runs libraries that are already available on your machine.
# - It always runs zrlog benchmark from current repo.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/benchmark_results"
mkdir -p "${OUT_DIR}"

run_cmd() {
  local name="$1"
  shift
  echo "[RUN] ${name}: $*"
  ("$@") | tee "${OUT_DIR}/${name}.log"
}

cd "${ROOT_DIR}"

echo "== System Info ==" | tee "${OUT_DIR}/system.txt"
{
  uname -a
  echo
  lscpu 2>/dev/null || true
  echo
  g++ --version || true
} >> "${OUT_DIR}/system.txt"

# 1) zrlog (always)
make -j"$(nproc)"
run_cmd zrlog "${ROOT_DIR}/benchmark_zrlog" 1 0

# 2) optional external libs (run only when user provides binaries)
for lib in spdlog quill fmtlog nanolog; do
  bin="${ROOT_DIR}/third_party_bench/${lib}_bench"
  if [[ -x "${bin}" ]]; then
    run_cmd "${lib}" "${bin}"
  else
    echo "[SKIP] ${lib}: ${bin} not found or not executable" | tee -a "${OUT_DIR}/skipped.log"
  fi
done

echo

echo "Done. Outputs: ${OUT_DIR}"
