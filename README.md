# zrlog

A C++17 asynchronous logging library focused on **ultra-low frontend latency** and **very high throughput**.

> 中文说明：`zrlog` 是一个面向延迟敏感与高并发场景的 C++17 异步日志库。v1.x 与 v2.x 在设计与依赖上有重要区别，请务必查看下文的 IMPORTANT NOTICE。

---

## IMPORTANT NOTICE (Required)

- `v1.x` is a single-header, zero-dependency implementation (single-header, no external dependencies).  
- `v2.x` introduces a dependency on the fmt library (fmtlib) for backend compile-time formatting (`FMT_COMPILE`).

Note: If you require a zero-dependency deployment, continue to use `v1.x`. If you accept the `fmt` dependency, `v2.x` provides significantly lower frontend latency and higher overall throughput.

---

## Project Overview

`zrlog` v2.x moves expensive work (formatting + syscalls) off the hot path. The frontend **serializes parameters in binary** and appends them to a per-thread SPSC ring buffer; a background consumer thread **formats using `FMT_COMPILE`** and performs batched I/O (`writev`) to achieve maximum throughput.

Core design goals:

- Minimize the cost of the logging call in application threads
- Push formatting and syscall work to a single background thread
- Keep memory and CPU overhead predictable and cache-friendly

---

## Key Features (v2.x)

- Single-header integration for the library (include `zrlog.h`), but **v2.x depends on fmtlib** for formatting.
- Per-thread lock-free `ThreadBuffer` (SPSC) to avoid contention.
- Binary entry format in the frontend (no formatting on hot path).
- Backend `FMT_COMPILE`-based formatting and `writev`/batched I/O.
- High-resolution TSC-based clock (with calibration).
- Configurable buffer-full policies: `Discard` / `Retry` / `Block`.
- Cross-platform fast paths (Linux / Windows) and architecture-specific optimizations (x86_64 / aarch64).

---

## Repository Layout

```
zrlog/
 ├── zrlog.h              # core header (v2.x uses fmt)
 ├── benchmark_zrlog.cpp  # benchmark & stress test
 ├── CMakeLists.txt
 ├── Makefile
 └── README.md
```

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

## Quick Start (v2.x — requires fmt)

> NOTE: v2.x uses `fmt` (`FMT_COMPILE`) for backend high-performance formatting. Ensure `fmt` headers (and library if linking) are available at build time.

### Minimal example

```cpp
#include "zrlog.h"   // v2.x zrlog.h uses fmt-style placeholders {}

int main() {
    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = "app.log";
    cfg.level = zrlog::LogLevel::INFO;
    cfg.thread_buffer_size = 1 * 1024 * 1024;   // per-thread 1MB
    cfg.io_buffer_size     = 1024 * 1024;       // backend I/O buffer
    cfg.buffer_full_policy = zrlog::BufferFullPolicy::Discard;

    if (!ZRLOG_INIT_CONF(cfg)) {
        return 1;
    }

    // fmt-style placeholders {}
    ZRLOG_INFO("service started, pid={}", 1234);
    ZRLOG_WARN("queue depth={}", 42);

    std::string s = "hello";
    ZRLOG_INFO("message: {}", s);

    // ensure backend flushes before exit
    ZRLOG_FINI();
    return 0;
}
```

### Behavioral notes

- Logging macros use **`fmt`-style `{}`** placeholders (not `printf`-style `%` tokens).  
- **Frontend does not format** strings — it serializes arguments and appends entries to a per-thread buffer. The background consumer does formatting using `FMT_COMPILE`.  
- Always call `ZRLOG_FINI()` on shutdown so buffered entries are formatted and flushed.

### Compilation (common cases)

#### 1) Header-only `fmt` (preferred for simple setups)

If `fmt` is used header-only or installed so that headers are discoverable:

```bash
g++ -O3 -std=c++17 -pthread -I/path/to/fmt/include your_program.cpp -o your_program
```

If your system already provides `fmt` headers:

```bash
g++ -O3 -std=c++17 -pthread your_program.cpp -o your_program
```

#### 2) Linking libfmt (if installed as a library)

If you need to link the compiled `fmt` library:

```bash
g++ -O3 -std=c++17 -pthread your_program.cpp -lfmt -o your_program
# or with pkg-config
g++ -O3 -std=c++17 -pthread `pkg-config --cflags fmt` your_program.cpp `pkg-config --libs fmt` -o your_program
```

### Common build/runtime errors

- `fatal error: fmt/compile.h: No such file or directory` — add `-I` to point to `fmt` headers or install `fmt`.
- Linker errors for `-lfmt` — install and link `fmt` correctly or use header-only mode.
- Using `%d`/`%s` style placeholders — replace them with `{}` style for all macros.

---

## Configuration Overview

```text
zrlog::Config {
    LogLevel level;
    AppenderType appender;      // File / Console
    std::string filename;
    size_t thread_buffer_size;  // per-thread ring buffer size (bytes)
    size_t io_buffer_size;      // backend I/O buffer (bytes)
    BufferFullPolicy buffer_full_policy; // Discard | Retry | Block
    int buffer_full_retry_count; // used when policy == Retry
    // other tuning params: TSC calibration, writev batch size, flush interval, etc.
}
```

Recommended defaults for many services:

- `thread_buffer_size = 1MB`
- `io_buffer_size = 1MB
- `buffer_full_policy = Discard` (latency-sensitive) or `Retry` (balanced)

---

## Log Levels

- TRACE
- DEBUG
- INFO
- WARN
- ERR
- FATAL

Compile-time filtering may be supported (depending on `zrlog.h` macros).

---

## Buffer Full Policy

- **Discard**: drop message immediately — lowest frontend latency.  
- **Retry**: retry a configured number of times before dropping.  
- **Block**: block until space available — guarantees delivery at cost of increased latency.

Choose per your latency vs durability needs.

---

## Backend & Performance Design

- Frontend does minimal work: timestamp (TSC), parameter serialization, and ring-buffer append.  
- Backend formats entries using `FMT_COMPILE` and issues batched `writev` calls — fewer syscalls and higher throughput.  
- SPSC per-thread buffers minimize contention and improve cache locality.  
- TSC-based timestamping reduces clock overhead (calibration available).

---

## Benchmark

A `benchmark_zrlog` program is included to measure:

- clock overhead (TSC vs system clock)
- frontend latency percentiles (p50/p90/p99)
- single-thread & multi-thread throughput

Run:

```bash
./benchmark_zrlog [thread_buffer_size_MB] [buffer_full_policy]
# e.g. ./benchmark_zrlog 1.0 0
```

---

## Migration: v1.x → v2.x

- **Dependency**: v2.x requires `fmt` (v1.x did not).  
- **API**: Most logging macros remain compatible, but placeholders are `fmt`-style `{}`.  
- **Semantics**: Frontend is faster (no formatting) and backend formatting timing may differ (log text appears when backend formats/flushes).

If zero-dependency is critical, keep using v1.x.

---

## Important Performance Note (appendix)

**Special note**: In our benchmark suite, `zrlog v2.x` shows **better frontend latency and higher overall throughput** than `fmtlog` under the same hardware, compiler flags, and workload profile. To make comparisons reproducible, always publish:

- benchmark commands & parameters
- hardware (CPU model, frequency, core count, caches)
- OS and kernel version
- compiler + flags (e.g. `g++ -O3 -march=native -std=c++17 -pthread`)
- exact library versions/commits
- metrics: p50/p90/p99, stable throughput (logs/s), CPU usage, memory usage
- whether TSC/writev/batching were enabled

Suggested presentation (table or chart) for fairness:

| Library      | p50 latency | p99 latency | Throughput (logs/s) | Test hardware |
|-------------:|:-----------:|:-----------:|:-------------------:|:-------------:|
| zrlog v2.x   | — ns        | — ns        | —                   | CPU, cores    |
| fmtlog       | — ns        | — ns        | —                   | same hardware |

Include raw benchmark scripts and outputs for verification.

---

## Notes & Gotchas

- Always call `ZRLOG_FINI()` to flush and drain backend buffers before exit.  
- v1.x remains available for zero-dependency needs. v2.x trades a small dependency for considerably better frontend latency and overall throughput.  
- When tuning for production, measure with your real workload and enable/disable TSC or writev batching as appropriate.

---

## License

MIT License.

---

## Contributing

Contributions, bug reports, and performance improvements are welcome.

When submitting performance-related changes, please provide:

1. CPU model & core count  
2. Compiler & flags  
3. Benchmark commands and raw outputs (p50/p90/p99, throughput)

