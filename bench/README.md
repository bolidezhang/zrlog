# Cross-library benchmark

Compares zrlog against spdlog, fmtlog, quill and NanoLog on frontend latency and
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
  the queue fills (zrlog Discard, fmtlog discard, quill blocks after growing
  to its cap, NanoLog blocks), so the
  numbers describe "logs accepted per second", not a lossless rate.
- Queues are sized equally (8 MB) across zrlog and fmtlog so capacity does not
  confound the comparison. For fmtlog this is set via
  `-DFMTLOG_BENCH_QUEUE_MB=8` **on the compiler command line** — `fmtlog.cc` is
  a separate TU that allocates `ThreadBuffer`, so defining it in only one TU
  would be an ODR violation and a heap overflow.
- `run_win.ps1` runs several rounds, **rotates the library order every round**
  to spread cold-cache bias, and reports the median. Use ≥9 rounds: single-run
  spread on this machine is larger than any code difference measured so far.

## Requirements

Third-party sources are not vendored. `bench/CMakeLists.txt` fetches them at
configure time (spdlog v1.17.0, fmtlog main, quill master, NanoLog master), so the simplest
route needs only network access:

```sh
cmake -S . -B build-bench -DZRLOG_BUILD_CROSS_BENCH=ON
cmake --build build-bench --config Release
```

To use pre-cloned sources instead (offline / slow network), point at a root
that holds `spdlog/`, `fmtlog/`, `quill/` and `nanolog/`:

```sh
cmake -S . -B build-bench -DZRLOG_BUILD_CROSS_BENCH=ON \
      -DZRLOG_BENCH_FETCH_DEPS=OFF -DZRLOG_BENCH_DEPS=/path/to/deps
```

Per-dependency overrides also work: `-DFETCHCONTENT_SOURCE_DIR_SPDLOG=...`,
`_FMTLOG=...`, `_NANOLOG=...`.

Expected layout of a pre-cloned deps root:

```
<deps>/
  spdlog/include/            gabime/spdlog v1.x
  fmtlog/                    MengRao/fmtlog (fmtlog.cc)
  quill/include/             odygrd/quill
  nanolog/runtime/           PlatformLab/NanoLog, after `make libNanoLog.a`
```

fmtlog's own fmt submodule is empty upstream; the CMake build reuses the fmt
bundled with zrlog. quill is header-only and cross-platform. NanoLog is
Linux-only and is skipped on Windows.

quill runs with a tuned configuration (the only library here that deviates
from its defaults):

- `initial_queue_capacity = 32 MB`. The stock 128 KB queue reallocates
  several times during a latency run, and each reallocation blocks the
  logging call for ~1.5-2 ms, which lands in max and drags avg up.
- `log_timestamp_ordering_grace_period = 0` (stock is 5 us), which lets the
  backend drain sooner.
- `UnboundedBlocking` (the stock default) - `BoundedBlocking` blocks the
  producer when full and made the 16M-message throughput test take 28 s.

Measured effect of this tuning (median of 7 rounds, same machine):

| config | avg | p50 | p99 | max | M logs/s |
|---|---|---|---|---|---|
| tuned | 20.7 | 20.0 | 28.0 | 46 us | 138.4 |
| stock | 49.9 | 26.7 | 36.7 | 2386 us | 129.6 |

Stock is kept under `results_win_quill_default/` for reference. Quote the
tuned numbers when comparing against zrlog/fmtlog (which also run tuned
configurations for their benchmarks), and say so explicitly when publishing.

Binaries land in `build-bench/bench/bin/<config>/` (multi-config generators
add the config directory).

## Windows

```bat
rem deps default to D:\work\zrlog-bench; override if yours differ
set ZRLOG_BENCH_DEPS=D:\path\to\deps
bench\build_bench.bat
powershell -File bench\run_win.ps1 -Rounds 7 -Threads 4 -Per 4000000
python bench\aggregate.py bench\bin\results
```

`run_win.ps1` auto-detects the P-cores (via `GetLogicalProcessorInformationEx`
and `EfficiencyClass`) and pins one logical CPU per physical core. Pass
`-Cpus "0,10,12,14"` to override. On this dev machine the hard-coded default
that used to be here (`2,3,4,5`) turned out to be **E-cores**, which
understated every library's absolute numbers — see `FINDINGS.md`, caveat 5.

`-BurnMs` (default 100) burns the CPU before the latency loop. This is
required: without it zrlog's p50 lands at either ~20 or ~50 ns with no code
change. quill and fmtlog are unaffected.

On hybrid CPUs (P-core + E-core) always pin to P-cores — an E-core can halve
throughput and reverse rankings.

## Linux

```sh
# 1) NanoLog runtime first (Linux only)
git clone https://github.com/PlatformLab/NanoLog
make -C NanoLog/runtime libNanoLog.a

# 2) build (spdlog/fmtlog/quill/nanolog auto-skipped if absent)
export ZRLOG_BENCH_DEPS=/path/to/deps   # holds spdlog/, fmtlog/, quill/, nanolog/
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
