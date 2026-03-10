# zrlog

A C++17 single-header asynchronous logging library focused on **ultra-low latency** and **high throughput**.

---

## Why zrlog

- **Single-header integration**: just include `zrlog.h`.
- **Asynchronous architecture**: producer threads serialize quickly; a backend thread formats and flushes.
- **Per-thread ring buffer**: reduces contention in multi-thread logging.
- **Configurable backpressure policy** when thread buffer is full.
- **Cross-platform branches**: Linux / Windows, with x86_64/aarch64 specific fast paths.

---

## Repository Layout

- `zrlog.h` — core library implementation (single header)
- `benchmark_zrlog.cpp` — benchmark & stress test
- `Makefile` / `CMakeLists.txt` — Linux build entry
- `zrlog.vcxproj` / `zrlog.sln` — Visual Studio project files

---

## Build

### Option A: Makefile

```bash
make
```

Binary output: `benchmark_zrlog`

### Option B: CMake

```bash
cmake -S . -B build
cmake --build build -j
```

---

## Quick Start

```cpp
#include "zrlog.h"

int main() {
    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = "app.log";
    cfg.level = zrlog::LogLevel::INFO;
    cfg.io_buffer_size = 1024 * 256;      // backend I/O buffer
    cfg.thread_buffer_size = 1024 * 1024; // per-thread ring buffer
    cfg.buffer_full_policy = zrlog::BufferFullPolicy::Discard;

    if (!ZRLOG_INIT_CONF(cfg)) {
        return 1;
    }

    ZRLOG_INFO("service started, pid=%d", 1234);
    ZRLOG_WARN("queue depth=%d", 42);
    ZRLOG_ERROR("db timeout, code=%d", -1);

    ZRLOG_FINI();
    return 0;
}
```

---

## Log Levels

`zrlog::LogLevel`:

- `TRACE`
- `DEBUG`
- `INFO`
- `WARN`
- `ERR`
- `FATAL`

---

## Buffer Full Policy

`zrlog::BufferFullPolicy`:

- `Discard` (`0`): drop immediately, best latency
- `Block` (`1`): block until there is space
- `Retry` (`2`): retry `buffer_full_retry_count` times, then drop

Recommended defaults:

- latency-first services: `Discard`
- reliability-first offline jobs: `Block`
- balanced online services: `Retry`

---

## Benchmark

Run:

```bash
./benchmark_zrlog [thread_buffer_size_MB] [buffer_full_policy]
```

Arguments:

- `thread_buffer_size_MB`: per-thread buffer size in MB (float, default `1.0`)
- `buffer_full_policy`: `0|1|2` => `Discard|Block|Retry`

The benchmark prints:

- clock call overhead
- frontend log-call latency percentiles
- single-thread throughput
- multi-thread throughput

---

## Notes

- Always call `ZRLOG_FINI()` before program exit to drain and flush remaining logs.
- Final message formatting uses `snprintf`; ensure format string and argument types match.

---

## Contributing

Issues and pull requests are welcome.

If you plan to benchmark changes, please include:

1. CPU model / core count
2. compiler & flags
3. benchmark command and key output metrics

