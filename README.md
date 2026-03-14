# zrlog

**zrlog** is a C++17 asynchronous logging library engineered for **latency-sensitive and high-concurrency systems**.
It focuses on extremely low frontend latency and very high throughput.

v1.x and v2.x differ significantly in design and dependencies — please read the **IMPORTANT NOTICE** below before using.

---

## ⚠️ IMPORTANT NOTICE: Version Selection

zrlog is maintained in two separate branches to cater to different project needs. Please checkout the appropriate branch:

*   **[v1.x Branch] Pure & Lightweight (Single-header)** 👉 `git checkout v1.x`
    *   **Features**: Single-header, **Zero third-party dependencies**.
    *   **Use Case**: Projects that need a drop-in logging solution without pulling in external libraries.
    *   **Format Mechanism**: Runtime formatting (supports traditional `printf`-style placeholders like `%d`, `%s`).
*   **[v2.x Branch] Extreme Performance (Current)** 👉 `git checkout v2.x`
    *   **Features**: Integrates the modern C++ formatting library **[`fmtlib`](https://github.com/fmtlib/fmt)** (Header-Only mode).
    *   **Use Case**: Modern C++ projects demanding the absolute highest performance, safety, and lowest latency.
    *   **Format Mechanism**: Leverages `fmtlib`'s `FMT_COMPILE` to shift log format parsing from runtime to **compile-time**.

> **🚨 BREAKING CHANGE in v2.x**: 
> v2.x is **NOT backward compatible** with v1.x. It entirely drops support for `printf`-style formatting. You **MUST** use `fmtlib` syntax (i.e., `{}` placeholders). If upgrading from v1.x, you must update all your format strings.

---

## 🏆 Performance Benchmarks

zrlog has been rigorously tested against industry-standard logging libraries. It sits comfortably in the absolute top tier of low-latency loggers:

*   **Beats `fmtlog`**: Consistently outperforms `fmtlog` in frontend latency (delivering lower **Average**, **P50**, and **MIN** latency). Moreover, its maximum **throughput is significantly higher** than `fmtlog`.
*   **Top-Tier Class**: Belongs to the same nanosecond-level elite tier as `nanolog` and `fmtlog`.
*   **Decimates Traditional Loggers**: Achieves **10x - 50x higher performance** (latency and throughput) compared to mainstream loggers like `spdlog`. It is an ideal replacement for High-Frequency Trading (HFT) and extreme low-latency backends.

### 📊 Running the Benchmark

You can easily compile and run the included `benchmark_zrlog.cpp` to verify the performance on your own hardware.

**1. Compile the benchmark:**
Ensure you have cloned or downloaded `fmtlib`. Replace `/path/to/fmt/include` with your actual `fmt` header path.
```bash
g++ -O3 -march=native -flto -std=c++17 -pthread -I./fmt/include benchmark_zrlog.cpp -o benchmark_zrlog
```

**2. Run the benchmark:**
Execute the compiled binary. By default, it uses a 1MB thread buffer and the `Discard` policy.
```bash
./benchmark_zrlog
```

*(Optional)* You can tune the benchmark by passing arguments:
```bash
# Usage: ./benchmark_zrlog [thread_buffer_size_MB] [buffer_full_policy]
# buffer_full_policy: 0 = Discard, 1 = Block, 2 = Retry

# Example: Run with a 2.5MB thread buffer and 'Retry' policy
./benchmark_zrlog 2.5 2
```

**The benchmark suite executes the following tests:**
1.  **Frontend Latency Test**: Measures the ns-level overhead of a single log call (Avg, MIN, MAX, P50, P90, P99, P99.9).
2.  **Throughput Test**: Measures Million logs/sec (M logs/sec) in both single-threaded and multi-threaded scenarios.
3.  **Message Type Latency**: Compares the serialization overhead of pure integers, strings, and mixed types.
4.  **Clock Performance**: Compares the speed of the hardware `TscClock` vs the standard `std::chrono::system_clock`.

---

## ✨ Enterprise-Grade Features (v2.3+)

- **🚀 Compile-Time Formatting (`FMT_COMPILE`)**: Seamlessly integrates `fmtlib`. Macros automatically extract format strings and generate ASTs, allowing the backend to execute highly optimized assembly instructions.
- **⚡ Lock-Free Frontend**: Uses a `ThreadLocal` ring buffer. 99.9% of log calls pass through with zero cost, completely avoiding cross-core bus traffic, locks, and atomic barriers.
- **🛡️ Crash Safe (Emergency Drain)**: Built-in POSIX signal handler (`SIGSEGV`, `SIGABRT`, etc.). Blocks the dying thread while forcing the background consumer to immediately flush all remaining in-memory logs to disk before a core dump. **Never lose the critical log right before a crash again.**
- **🔄 Zero-Allocation Log Rotation**: Natively supports file size-based rotation (`RotatingFile`). Built with stack-allocated buffers and `access()` short-circuiting to guarantee zero heap fragmentation and zero latency spikes during file roll-overs.
- **⏱️ Hardware TSC Timestamp**: Bypasses `std::chrono::system_clock` by reading the CPU's `rdtsc` instruction directly for ultra-fast time retrieval.
- **🛡️ Cache-Line Isolation**: Core data structures are strictly aligned using `alignas(CACHE_LINE_SIZE)` to entirely eliminate False Sharing across threads.
- **🧵 Zero-Cost Static Strings**: Introduces the `_sl` literal suffix and `zrlog::literal` wrapper. This intercepts string constants at compile time, eliminating frontend `strlen` and deep copy overhead.
- **🧠 Smart Consumer Scheduling**: The background consumer uses Epoch Scanning and active/inactive partition algorithms, batch-committing cursors to minimize L1 cache invalidation.

---

## 🛠️ Build & Dependencies (v2.x)

- **Standard**: C++17 or above.
- **Dependencies**: `fmtlib` (Header-Only mode is enabled internally via `#define FMT_HEADER_ONLY`).
- **Platforms**: Linux (x86_64 / aarch64), Windows (MSVC), macOS.

**Compilation Example (GCC/Clang):**
# Don't forget to include the fmtlib path
g++ -O3 -march=native -flto -std=c++17 -pthread -I/path/to/fmt/include main.cpp -o app

---

## 🚀 Quick Start

### Basic Usage & Crash Handler

*Note: Use `{}` placeholders for v2.x.*

```cpp
#include "zrlog.h"

int main() {
    // 1. (Highly Recommended) Install crash handler to catch SIGSEGV/SIGABRT and flush logs
    zrlog::install_crash_handler();

    // 2. Configure logger (Rotating File, 100MB per file, keep 5 files, Level: INFO)
    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::RotatingFile;
    cfg.filename = "app_run.log";
    cfg.rotating_file_size = 100 * 1024 * 1024;
    cfg.rotating_max_files = 5;
    cfg.level = zrlog::LogLevel::INFO;
    
    ZRLOG_INIT_CONF(cfg);

    // 3. Write logs (Supports modern fmt syntax)
    ZRLOG_INFO("Server started on port {}", 8080);
    ZRLOG_WARN("User {} login failed, attempt: {}", "admin", 3);

    // 4. Shutdown and flush before exit
    ZRLOG_FINI();
    return 0;
}
```

### ⚡ Extreme Optimization: Static String Literals

Normally, `ZRLOG_INFO("Error: {}", "Timeout")` forces the frontend to execute `strlen("Timeout")` and deep-copy the string. 
zrlog v2.x offers the **`_sl`** literal suffix and **`zrlog::literal()`** wrapper to achieve **zero-latency and zero-copy** for constant strings:

using namespace zrlog::literals; // Import _sl suffix

// Recommended 1: Using the _sl suffix
ZRLOG_INFO("System status: {}", "Database connection lost"_sl);

// Recommended 2: Using the zrlog::literal wrapper
ZRLOG_ERROR("Module load error: {}", zrlog::literal("CacheManager"));

### ⚠️ Static vs. Dynamic Format Strings

zrlog achieves extreme performance by binding format strings to log sites at **compile time**. Therefore, **you cannot pass dynamic variables as the format string in standard macros.**

std::string my_msg = "User disconnected";

// ❌ COMPILE ERROR: zrlog actively blocks this to prevent syntax issues and performance drops.
ZRLOG_INFO(my_msg); 

// ✅ Correct (Zero-cost compile-time AST + variable argument):
ZRLOG_INFO("{}", my_msg);

// ✅ Correct (Runtime Format Parsing via DYN macros):
// Use this ONLY if the format string itself changes at runtime (e.g., loaded from configs).
ZRLOG_DYN_INFO(my_msg); 

---

## ⚙️ Core Concepts & Configuration

### 1. Log Levels
zrlog provides 6 severity levels. You can configure the global minimum log level during initialization. Any log statement below this level is **completely bypassed at the macro level**, resulting in zero CPU overhead.

Available levels and their corresponding macros:
*   `TRACE` ➔ `ZRLOG_TRACE(...)` / `ZRLOG_DYN_TRACE(...)`
*   `DEBUG` ➔ `ZRLOG_DEBUG(...)` / `ZRLOG_DYN_DEBUG(...)`
*   `INFO`  ➔ `ZRLOG_INFO(...)`  / `ZRLOG_DYN_INFO(...)`
*   `WARN`  ➔ `ZRLOG_WARN(...)`  / `ZRLOG_DYN_WARN(...)`
*   `ERROR` ➔ `ZRLOG_ERROR(...)` / `ZRLOG_DYN_ERROR(...)`
*   `FATAL` ➔ `ZRLOG_FATAL(...)` / `ZRLOG_DYN_FATAL(...)`

### 2. Appenders (Output Destinations)
Set `config.appender` to one of the following:
*   `zrlog::AppenderType::Console`: Output to `stdout` (extremely fast, suitable for Docker/Kubernetes).
*   `zrlog::AppenderType::File`: Append to a single file indefinitely.
*   `zrlog::AppenderType::RotatingFile`: Automatically roll over files when they reach `rotating_file_size`. Keeps up to `rotating_max_files` backups (e.g., `app.log.1`, `app.log.2`).

### 3. Buffer Full Policy (Crucial for HFT)
Since zrlog is an asynchronous logger, the frontend (business thread) writes to a thread-local RingBuffer, and a background thread reads it and writes to the disk. 
**What happens when your business thread produces logs faster than the disk can write, and the RingBuffer gets full?** 

zrlog provides three policies via `zrlog::BufferFullPolicy` to handle this scenario, allowing you to choose between latency and data completeness:

1.  **`Discard` (Extreme Low Latency)**: 
    *   *Behavior*: Instantly drops the new log entry if the buffer is full.
    *   *Use Case*: Algorithmic trading (HFT) and real-time systems where avoiding frontend latency spikes is vastly more important than losing a few debug logs. Business threads are **never** blocked.
2.  **`Block` (Zero Data Loss)**: 
    *   *Behavior*: The frontend thread will spin and yield (using an adaptive backoff algorithm) until the background thread clears enough space.
    *   *Use Case*: Financial auditing, transaction logging, or scenarios where every single log entry is mission-critical.
3.  **`Retry` (Default - Balanced Approach)**: 
    *   *Behavior*: Spins and retries for a limited number of times (configured via `config.buffer_full_retry_count`, default 256). If the buffer is still full after a few microseconds of pure CPU `pause`, it drops the log instead of triggering expensive OS scheduling (`yield`).
    *   *Use Case*: Recommended for most backend services to smooth out micro-bursts of logs safely.

---

## 🧠 Design Philosophy

1. **Why zero system calls on the frontend?**
   Business threads only write the raw binary representation of their arguments (not formatted text) into a lock-free Thread-Local RingBuffer. This completely bypasses global mutexes, `snprintf`, and string concatenation overhead.
2. **How do we prevent Cache-Line Bouncing?**
   In the `ThreadBuffer`, producer cursors (`write_index_`) and consumer cursors (`read_index_`) are strictly separated by `alignas(CACHE_LINE_SIZE)`. This guarantees a near 100% L1/L2 cache hit rate by physically isolating atomic variables.

---

## ❓ FAQ (Frequently Asked Questions)

### Q1: What happens if my application crashes (e.g., Segmentation Fault)? Do I lose logs?
**Not if you use `zrlog::install_crash_handler()`!** 
By calling this at the start of your `main()`, zrlog hooks into POSIX signals (`SIGSEGV`, `SIGABRT`, etc.). When a crash occurs, zrlog blocks the dying thread and forces the background consumer to immediately flush all remaining in-memory logs to the disk. After the flush (or a 3-second timeout), it gracefully restores the default signal handler to generate the core dump.

### Q2: Why is `std::chrono::system_clock` replaced with `TscClock`?
Getting the current time is notoriously slow (often 20-50ns even with vDSO). For a logger aiming for sub-30ns frontend latency, this is a massive bottleneck. zrlog reads the CPU's Time Stamp Counter (`rdtsc` instruction in x86), which takes ~1-2ns, and syncs it with the system clock periodically in the background thread.

### Q3: What happens if a business thread exits/dies? Do I lose the logs currently in its thread-local buffer?
**No.** zrlog uses a `thread_local ThreadBufferDestroyer`. When a thread exits, the destructor is automatically triggered. It marks the thread's buffer for deallocation and signals the background consumer. The consumer will completely drain the remaining logs to the disk before safely deleting the buffer memory. **No memory leaks, no lost logs.**

### Q4: Why is `_sl` or `zrlog::literal` so much faster than a normal string?
When you log a normal string `ZRLOG_INFO("{}", "hello")`, the frontend must calculate its length (`strlen`) and perform a deep copy of the characters ("hello") into the ring buffer. 
By using `ZRLOG_INFO("{}", "hello"_sl)`, you are telling zrlog this string lives in the read-only data segment (constant pool). zrlog simply copies the pointer and its compile-time length (16 bytes total) into the buffer. The background thread later dereferences the pointer directly. **Zero `strlen`, zero deep copy.**

### Q5: Can I log custom classes or structs?
**Yes.** Because zrlog v2.x natively integrates `fmtlib`, any custom type that has an implemented `fmt::formatter` will work seamlessly. You pass your object just like you would to `fmt::print()`, and zrlog will handle the serialization. *(Note: Ensure the object's state is safely copied if it mutates after the log call, as formatting happens asynchronously).*

### Q6: Why do I get a compilation error saying `FMT_COMPILE` or `fmt` is not found?
Ensure you have downloaded `fmtlib` and added its `include` directory to your compiler's include path (e.g., `-I/path/to/fmt/include`). zrlog v2.x strictly requires `fmtlib` to unlock its compile-time formatting capabilities.