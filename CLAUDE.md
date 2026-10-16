# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

zrlog 是一个面向低延迟、高并发系统的 C++17 异步日志库，核心目标是纳秒级前端延迟与极高吞吐。整个库的实现都在单个头文件 `zrlog.h` 中（约 2200 行），无 .cpp 源文件。

**版本分支注意**：仓库以两个不兼容的分支维护：
- `v1.x`：单头文件、零第三方依赖、printf 风格（`%d`/`%s`）
- `v2.x`（当前主线）：集成 fmtlib（header-only），使用 `{}` 占位符 + `FMT_COMPILE` 编译期格式化

v2.x **不兼容** v1.x，所有格式串必须使用 fmt 语法。修改 `zrlog.h` 时需确认当前分支。

## 构建与测试

唯一可构建目标是性能基准 `benchmark_zrlog.cpp`（**本仓库没有单元测试**，功能验证靠临时测试程序）：

```bash
# fmt 是 git submodule，首次 checkout 后需初始化
git submodule update --init

# 方式一：Makefile（默认 Release，-O3 -march=native，DEBUG=YES 可切换）
make && ./benchmark_zrlog

# 方式二：CMake（默认 Release，自动加 -O3 -march=native -flto）
cmake -B build && cmake --build build && ./build/benchmark_zrlog

# 方式三：直接编译
g++ -O3 -march=native -flto -std=c++17 -pthread -I./fmt/include benchmark_zrlog.cpp -o benchmark_zrlog

# 运行基准：[线程缓冲区大小MB] [缓冲区满策略 0=Discard 1=Block 2=Retry]
./benchmark_zrlog 2.5 2
```

**⚠️ Makefile 陷阱**：`%.o: %.h` 是一条空规则，实际上**不会跟踪 `zrlog.h` 的改动**（pattern 依赖的是不存在的同名 .h）。改完 `zrlog.h` 后必须 `rm -f *.o benchmark_zrlog && make` 强制重编，否则跑的是旧二进制——已经踩过一次：回归测试对着旧二进制跑出了误导性结果。

基准套件：前端延迟（Avg/P50/P99/P99.9）、单/多线程吞吐、不同参数类型序列化开销、TscClock vs system_clock。改动前后端路径后应跑基准对比。注意本开发机（见下）单次运行的吞吐波动可达 ±3 倍，跨次对比只信 p50 类稳定指标。

Windows 侧有 `zrlog.sln` / `zrlog.vcxproj`（MSVC 工程）。

## 架构

严格前后端解耦是全部设计的核心：

```
业务线程（前端） → TLS RingBuffer（每线程 1MB，无锁 SPSC） → 后台线程（NanoLogger）
                                                                    ↓
                                                        IOBuffer（4MB）→ 磁盘（可轮转）
```

**前端（业务线程内执行）**：零解析、零字符串格式化、零系统调用。读取 TSC 硬件时间戳（`rdtsc`，~1-2ns），把原始参数序列化为稠密二进制 payload 推入线程本地 RingBuffer。

**后端（专用后台线程）**：用 Epoch 轮询 + active/inactive 分区算法扫描所有活跃线程缓冲区，弹出二进制 payload，用编译期生成的 AST（`FMT_COMPILE`）解码为文本，写入大 IOBuffer 后批量落盘。

`zrlog.h` 内的关键组件（行号以当前 main 为准，改代码后记得同步）：
- `detail::SpinMutex` / `detail::TscClock`（~L330）：自旋锁与 TSC 时钟（定点乘法换算 + seqlock 锚点 + 每小时与系统时钟重同步）
- `util` 命名空间（~L473）：线程 ID、绑核、HugePage 内存块
- `LogLevel` / `AppenderType` / `BufferFullPolicy` / `Config`（L611-669）：公共配置接口
- `ILogAppender` 及子类 `FileLogAppender` / `RotatingFileLogAppender` / `ConsoleLogAppender`（L671-952）：输出目的地；轮转采用 O(1) 滑动窗口 + 孤儿文件清理 + 写失败退避
- `NanoLogger`（~L954）：单例核心，内含 `IOBuffer`、`LogMeta`、`LogEntryHeader`（16 字节）；后台消费线程、崩溃应急 flush（`emergency_flush`，3 秒超时兜底）都在这里
- `NanoLogger::ThreadBuffer`（~L1495）：每线程 SPSC 环形缓冲；生产者/消费者游标 `alignas(CACHE_LINE_SIZE)` 隔离防伪共享；生产者用本地缓存游标 `cached_read_index_` 避免跨核通信，消费者 4KB 批量提交游标
- `ThreadBufferDestroyer`（~L1742）：线程退出时标记缓冲区待释放，后台线程排空后才 delete——不丢日志不泄漏
- L2145 起为公共宏层：`ZRLOG_INIT_CONF` / `ZRLOG_FINI` / `ZRLOG_INFO` / `ZRLOG_DYN_*` 等

**平台抽象**：跨平台通过 `ZRLOG_OPEN`/`ZRLOG_WRITE`/`ZRLOG_CPU_PAUSE`/`ZRLOG_FAST_THREAD_LOCAL` 等宏完成（MSVC/POSIX/x86/aarch64 分支在 L94-194）。改动涉及系统调用或原子操作时，注意同时覆盖所有平台的宏分支。

**崩溃安全**：`zrlog::install_crash_handler()` 挂接 SIGSEGV/SIGABRT/SIGFPE/SIGILL；崩溃时阻塞垂死线程、强制后台线程排空内存中剩余日志（`crash_drain_wait_ms` 超时兜底），然后恢复默认处理器产生 core dump。已知限制：崩溃发生在后台线程自身时抢救必然失败（等待对象就是自己）。

## 关键设计决策（改动前先读）

- **消费者空闲等待用"持锁 notify"协议**：`idle_wait_flag_` 置位在 `idle_wait_mutex_` 锁内，`notify_consumer()` 持锁后再 notify——保证生产者看到 flag==true 时消费者必然已在 `wait_for` 中，通知不会丢（竞态 1）。生产者检查 flag 时消费者尚未置位的宽窗口（命中率 ~2%）**有意**由 `idle_wait_interval_us` 超时兜底，不要"顺手"加 wait_for 谓词去修它——曾实现后回退，结论见 git log 与提交 `67714a6` 的取舍注释。
- **Block/Retry 策略对超大单条直接放弃**：单条日志 ≥ 线程缓冲容量时永远无法入队，`push_log_entry` 直接 return false，防止业务线程永久自旋（曾为活锁 bug）。

## API 约定（修改宏或示例时必须遵守）

- 格式串必须是编译期字面量：`ZRLOG_INFO(my_msg)` 是故意编译报错的（`"" fmt_str ""` 拼接技巧），应写 `ZRLOG_INFO("{}", my_msg)`
- 裸 `char*`/`const char*` 参数被 `static_assert` 全面禁止（防异步格式化悬垂指针），用 `std::string_view` 或 `zrlog::literal`
- 常量字符串用 `"..."_sl` 字面量后缀或 `zrlog::literal()` 包装：只拷贝指针+编译期长度（16 字节），零 `strlen` 零深拷贝
- 格式串确需运行时确定时用 `ZRLOG_DYN_*` 宏（慢路径，`fmt::runtime`）。同一调用点的格式串**允许运行时更换**：调用点持有 FNV-1a 内容哈希缓存（`fmt_hash_cache`），哈希不一致时进锁内追加新 `LogMeta` 并原子切换 `log_id`（旧 meta 保留供缓冲区中历史条目渲染）。注意该路径每条日志都要算一次哈希（O(len)），且两个线程在同一调用点并发使用不同格式串时，个别条目可能按对方格式渲染（fmt 会抛 format_error，被 decoder 捕获输出 `[FMT_ERROR]` 行）
- fmtlib 以 `FMT_HEADER_ONLY` 方式使用，头文件路径 `fmt/include`（submodule）
