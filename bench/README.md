# Cross-library benchmark

Compares zrlog against spdlog, fmtlog and NanoLog on frontend latency and
throughput. Each library is compiled into its **own** executable because they
vendor different fmt versions and cannot coexist in one binary; the measurement
code in `bench_common.h` is shared so the methodology is identical.

## Measurement methodology

- Timestamps come from the TSC (`__rdtsc`), calibrated against `steady_clock`
  at startup, so percentiles are not quantized by the ~100 ns system clock.
- One timestamp pair per call; all samples are sorted offline to get
  percentiles (never averaged per round).
- The latency loop self-spins to throttle (no `sleep`): sleeping wakes with a
  C-state/cache penalty that shows up as a fake microsecond-scale p99.9 tail.
- The throughput loop is not throttled. Libraries differ in what happens when
  the queue fills (zrlog Discard, fmtlog discard, NanoLog blocks), so the
  numbers describe "logs accepted per second", not a lossless rate.
- `run_win.ps1` runs several rounds, **rotates the library order every round**
  to spread cold-cache bias, and reports the median.

## Requirements

Third-party sources are not vendored. Provide a directory containing:

```
<spdlog>/include/      gabime/spdlog v1.x
<fmtlog>/              MengRao/fmtlog (fmtlog.h, fmtlog-inl.h, fmtlog.cc)
```

NanoLog is Linux-only (POSIX AIO) and is skipped on Windows.

## Windows

```bat
rem deps default to D:\work\zrlog-bench; override if yours differ
set ZRLOG_BENCH_DEPS=D:\path\to\deps
bench\build_bench.bat
powershell -File bench\run_win.ps1 -Rounds 7 -Threads 4 -Per 4000000
python bench\aggregate.py bench\bin\results
```

`run_win.ps1` pins worker threads to `BENCH_CPUS` (default `2,3,4,5`). On hybrid
CPUs (P-core + E-core) always pin to P-cores - an E-core can halve throughput
and reverse rankings. Use `zrlog::util::get_performance_core_ids()` or any
topology tool to find them.

## Linux

```sh
# 1) NanoLog runtime first (Linux only)
git clone https://github.com/PlatformLab/NanoLog
make -C NanoLog/runtime libNanoLog.a

# 2) build (spdlog/fmtlog/nanolog auto-skipped if absent)
export ZRLOG_BENCH_DEPS=/path/to/deps   # holds spdlog/, fmtlog/, nanolog/
bench/build_bench.sh

# 3) run
bench/run_bench.sh 7 4 4000000
python3 bench/aggregate.py bench/results
```

## Reading the results

Latency percentiles are comparable within a single run. Absolute throughput
drifts with machine load: on this project a 32% background load moved zrlog
from ~377 to ~285 M logs/s while the untouched baseline moved identically, so
always compare against a baseline built from the same commit on the same idle
machine. Record CPU model, pinning and load alongside any table you publish.

## Recorded results

- `results/` - Ubuntu VM run (4 vCPU). NanoLog throughput there was unusable
  (0.08-15.9 M/s); the VM exposes its blocking-backpressure design.
- `results_win/`, `results_win_after/`, `results_win_final/` - Windows host
  runs at different code revisions. `env.txt` in each directory records the
  exact conditions and caveats.
