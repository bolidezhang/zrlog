# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

zrlog 是一个面向低延迟、高并发系统的 C++17 异步日志库，核心目标是纳秒级前端延迟与极高吞吐。整个库的实现都在单个头文件 `zrlog.h` 中（约 3060 行），无 .cpp 源文件。当前版本 2.5.1（`ZRLOG_VERSION`）。

**版本分支注意**：仓库以两个不兼容的分支维护：
- `v1.x`：单头文件、零第三方依赖、printf 风格（`%d`/`%s`）
- `v2.x`（当前主线）：集成 fmtlib（header-only），使用 `{}` 占位符 + `FMT_COMPILE` 编译期格式化

v2.x **不兼容** v1.x，所有格式串必须使用 fmt 语法。修改 `zrlog.h` 时需确认当前分支。

## 构建与测试

```bash
# fmt 是 git submodule，首次 checkout 后需初始化
git submodule update --init

# CMake：默认 Release，benchmark 与 tests 开启，跨库 bench 关闭
cmake -B build && cmake --build build --config Release

# 运行全部测试（ctest）
ctest --test-dir build -C Release --output-on-failure

# 运行单个测试（按名字正则）
ctest --test-dir build -C Release -R zrlog_poison_pill --output-on-failure

# 直接编译 benchmark（不经过 CMake 时）
g++ -O3 -march=native -flto -std=c++17 -pthread -I./fmt/include benchmark_zrlog.cpp -o benchmark_zrlog

# 运行基准：[线程缓冲区大小MB] [缓冲区满策略 0=Discard 1=Block 2=Retry] [hugepage 0/1]
./build/benchmark_zrlog 2.5 2
```

CMake 选项：`ZRLOG_BUILD_BENCHMARK`（默认 ON）、`ZRLOG_BUILD_TESTS`（默认 ON）、`ZRLOG_BUILD_CROSS_BENCH`（默认 OFF，配置时需联网克隆第三方库）。MSVC 下 `zrlog` INTERFACE target 自动带 `/utf-8`——fmt 12.x 的 Unicode 支持缺它会静态断言失败，手动编译时也要加。

**测试套件**（`tests/`，自研轻量框架，无第三方依赖，`TEST(name)` + `CHECK`/`CHECK_EQ`，失败抛异常、main 汇总返回非 0）：
- `zrlog_tests`（ctest 名 `zrlog_unit_tests`）：23 个功能用例，覆盖序列化/轮转/线程退出/DYN 等。新增用例直接在 `tests/zrlog_tests.cpp` 里加 `TEST()`
- `zrlog_exception_test`（`zrlog_exception_barrier`）：自定义 formatter 抛异常时进程须存活并输出 `[FMT_ERROR]`
- `zrlog_poison_test`（`zrlog_poison_pill`）：decoder 抛异常不得卡死后台线程与 `fini()`
- `zrlog_crash_test`（`zrlog_crash_flush`，**仅 Windows**）：CreateProcess 子进程触发 SIGSEGV，父进程校验崩溃前日志已落盘

测试刻意**不加** -O3/-flto（追求可调试与快速反馈）；激进优化只给 benchmark。测试写的日志文件统一落在构建目录（ctest 已设 WORKING_DIRECTORY），不污染源码树。

**⚠️ 构建注意**：改完 `zrlog.h` 后必须重新编译 benchmark 和测试（CMake 会自动跟踪头文件依赖），否则跑的是旧二进制——已经踩过一次：回归测试对着旧二进制跑出了误导性结果。

benchmark 套件：前端延迟（Avg/P50/P99/P99.9）、单/多线程吞吐、不同参数类型序列化开销、TscClock vs system_clock。改动前后端路径后应跑基准对比。注意本开发机单次运行的吞吐波动可达 ±3 倍，跨次对比只信 p50 类稳定指标。

## 跨库对比基准（bench/）

zrlog vs spdlog / fmtlog / NanoLog。**每个库编译成独立可执行文件**——各库自带的 fmt 版本不同，混进一个二进制会 ODR 冲突；测量代码共用 `bench/bench_common.h` 保证口径一致。

两条构建路线：
- **CMake（需联网）**：`cmake -B build-bench -DZRLOG_BUILD_CROSS_BENCH=ON`，FetchContent 拉取 spdlog v1.17.0 / fmtlog main / NanoLog master。离线用 `-DZRLOG_BENCH_FETCH_DEPS=OFF -DZRLOG_BENCH_DEPS=/path/to/deps`（目录下含 `spdlog/`、`fmtlog/`、`nanolog/`），或 `-DFETCHCONTENT_SOURCE_DIR_<SPDLOG|FMTLOG|NANOLOG>=...` 逐个指定。GitHub 直连被重置时用 `-DZRLOG_BENCH_GIT_MIRROR=https://gh-proxy.com/` 等镜像前缀
- **脚本（手动准备依赖）**：`ZRLOG_BENCH_DEPS` 环境变量指向依赖根（默认 `../zrlog-bench`），跑 `bench/build_bench.sh`（Windows 用 `build_bench.bat`），缺哪个库就跳过哪个。NanoLog 仅 Linux，需先 `make libNanoLog.a`；fmtlog 上游的 fmt submodule 是空的，复用 zrlog 自带的 fmt

运行与汇总：`bench/run_bench.sh [rounds] [threads] [per_thread]`（Windows：`run_win.ps1 -Rounds 7 -Threads 4 -Per 4000000`），`python bench/aggregate.py <results目录>` 取中位数。

**测量方法学的刻意取舍（不要"顺手修"）**：延迟循环用自旋节流而非 sleep（睡眠唤醒的 C-state/缓存惩罚会制造微秒级假 p99.9 尾部）；时间戳用 TSC 校准，样本离线排序取百分位；吞吐循环不节流，数字是"接受条数/秒"而非无损速率；每轮轮转库顺序均摊冷缓存偏置。混合架构（P/E 核）上必须用 `BENCH_CPUS` 绑到 P 核，否则排名会被 E 核拉偏。对比结论必须对着**同一提交、同一空闲机器**构建的基线，并随结果记录 CPU/绑核/负载（各 results 目录的 `env.txt`）。

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
- `SpinMutex`（L405）/ `TscClock`（L424）：自旋锁与 TSC 时钟（定点乘法换算 + seqlock 锚点 + 每小时与系统时钟重同步）。锚点字段是原子量，差值按有符号解释（前端条目可能早于上次 sync 锚点）；测率用 steady_clock（NTP 免疫），绝对锚点用 system_clock
- `util` 命名空间（L617 起）：线程 ID、绑核、P-core 拓扑探测、HugePage 内存块
- `LogLevel` / `AppenderType` / `BufferFullPolicy` / `Config`（L874-963）：公共配置接口
- `ILogAppender` 及子类 `FileLogAppender` / `RotatingFileLogAppender` / `ConsoleLogAppender`（L965 起）：输出目的地；轮转采用 O(1) 滑动窗口 + 孤儿文件清理 + 写失败退避（硬错误关 fd 降级 stderr，软错误保留 fd 等恢复）
- `NanoLogger`（L1318）：单例核心，内含 `IOBuffer`（L1346，支持大页 + 部分写保留/丢弃统计）、`LogMeta`、`LogEntryHeader`（16 字节，存原始 TSC 而非 ns）；后台消费线程、崩溃应急 flush（`emergency_flush`，有界 rdtsc 预算兜底）都在这里
- `NanoLogger::ThreadBuffer`（L2108）：每线程 SPSC 环形缓冲；生产者/消费者游标 `alignas(CACHE_LINE_SIZE)` 隔离防伪共享；生产者用本地缓存游标 `cached_read_index_` 避免跨核通信，消费者 4KB 批量提交游标；生产统计计数也放在生产者私有缓存行
- `ThreadBufferDestroyer`（L2373）：线程退出时冲刷统计、标记缓冲区待释放并置空 TLS 指针，后台线程排空后才 delete——不丢日志不泄漏不复用已释放内存
- L3010 起为公共宏层：`ZRLOG_INIT_CONF` / `ZRLOG_FINI` / `ZRLOG_INFO` / `ZRLOG_DYN_*` 等

**平台抽象**：跨平台通过 `ZRLOG_OPEN`/`ZRLOG_WRITE`/`ZRLOG_CPU_PAUSE`/`ZRLOG_FAST_THREAD_LOCAL` 等宏完成（MSVC/POSIX/x86/aarch64 分支在 L87-204）。改动涉及系统调用或原子操作时，注意同时覆盖所有平台的宏分支。

**崩溃安全**：`zrlog::install_crash_handler()`（L2986）挂接 SIGSEGV/SIGABRT/SIGFPE/SIGILL；崩溃时阻塞垂死线程、强制后台线程排空内存中剩余日志（`crash_drain_wait_ms` 超时兜底），然后恢复默认处理器产生 core dump。后台抢救是**有界** drain（rdtsc 预算，防其他线程持续产日志导致追不上），元数据同步用 try_lock（垂死线程可能持锁），退出前置停止标志让 Block/Retry 生产者退出自旋。信号处理器内只做 async-signal-safe 动作（原子标志 + rdtsc 忙等），实例指针与 ns/tick 在 install 时预缓存。已知限制：崩溃发生在后台线程自身时抢救必然失败（等待对象就是自己）。

## 关键设计决策（改动前先读）

- **消费者空闲等待用"持锁 notify"协议**：`idle_wait_flag_` 置位在 `idle_wait_mutex_` 锁内，`notify_consumer()` 持锁后再 notify——保证生产者看到 flag==true 时消费者必然已在 `wait_for` 中，通知不会丢（竞态 1）。生产者检查 flag 时消费者尚未置位的宽窗口（命中率 ~2%）**有意**由 `idle_wait_interval_us` 超时兜底，不要"顺手"加 wait_for 谓词去修它——曾实现后回退，结论见 git log 与提交 `67714a6` 的取舍注释。
- **Block/Retry 策略对超大单条直接放弃**：单条日志 ≥ 线程缓冲容量时永远无法入队，`push_log_entry` 直接 return false，防止业务线程永久自旋（曾为活锁 bug）。
- **先解码再推进游标 + 调用点兜底 catch（毒丸防护）**：消费循环里 `tb->consume()` 在 `meta.decoder()` **之后**调用。原因：解码参数是指向环形缓冲内部的零拷贝 `string_view`、header 按引用读取，若先 consume 发布 `read_index_`，生产者可能在 decoder 阻塞落盘期间绕回一整圈覆盖正在解码的条目（背压 + 慢盘下日志撕裂/时间戳跳变）。毒丸防护由"decoder 调用点包 try/catch(...) + 无论是否抛出都无条件 consume"保证——坏日志绝不重试（曾形成死循环卡死后台线程与 `fini()`，见 `a501692` 与 `zrlog_poison_test`）。改 decoder 或消费循环时务必同时保住这两点：解码期间游标不前进、consume 一定执行。
- **解码错误限流**：`[FMT_ERROR]` 行只输出前 100 条（`kMaxDecodeErrorLines`），超出只计数，防止坏 formatter 淹没日志文件；总数经 `decode_error_count()` 观测。
- **消费侧无锁提示 + 统计批量提交**：`meta_count_hint_`/`buffer_count_hint_` 让高频轮询先做无锁判断再决定是否抢自旋锁；消费统计在局部变量累加、每轮一次性 fetch_add。不要改回每条日志做原子加（x86 lock xadd ~20+ 周期，曾显著抬高开销，见 `d959794`）。前端生产统计同理：线程本地计数每 1024 条才汇总。
- **`force_flush_interval_ms = 0` 关闭周期 fsync**：日志仍靠空闲/缓冲满/退出路径 write 到 page cache，只是不再定期强制刷盘；慢盘/网络存储应保持默认 3000ms。

## API 约定（修改宏或示例时必须遵守）

- 格式串必须是编译期字面量：`ZRLOG_INFO(my_msg)` 是故意编译报错的（`"" fmt_str ""` 拼接技巧），应写 `ZRLOG_INFO("{}", my_msg)`
- 裸 `char*`/`const char*` 参数被 `static_assert` 全面禁止（防异步格式化悬垂指针），用 `std::string_view` 或 `zrlog::literal`
- 常量字符串用 `"..."_sl` 字面量后缀或 `zrlog::literal()` 包装：只拷贝指针+编译期长度（16 字节），零 `strlen` 零深拷贝
- 自定义类型参数必须"按值拷贝安全"（`detail::loggable_arg`：trivially copyable 或字符串类型）——前端逐位 memcpy 进缓冲，含堆内存的类型会在后端形成悬垂指针；确需支持时特化 `loggable_arg` 并自行保证安全
- 格式串确需运行时确定时用 `ZRLOG_DYN_*` 宏（慢路径，`fmt::runtime`）。同一调用点的格式串**允许运行时更换**：调用点持有单个打包原子量 `dyn_state`（高 32 位 `log_id` | 低 32 位 FNV-1a 内容哈希），一次 load 取一致快照；哈希不一致时进锁内经 `runtime_meta_index_`（内容哈希→候选 id，O(1) 摊还）按 (file, line, 格式内容) 查重复用或追加新 `LogMeta`（旧 meta 保留供缓冲区中历史条目渲染）。**不要拆回两个独立原子量**——并发换格式串会交错出"id 与哈希错配"并持续按错格式渲染。取舍：哈希截断 32 位（碰撞 ~2^-32，慢路径全内容比对兜底）；每条日志仍算一次 O(len) 哈希；两线程同调用点并发用不同格式串时个别条目仍可能按对方格式渲染（fmt 抛 format_error，decoder 捕获输出 `[FMT_ERROR]` 行）
- fmtlib 以 `FMT_HEADER_ONLY` 方式使用，头文件路径 `fmt/include`（submodule）
- 生命周期：`init()` 只在启动 logging 前调一次，**运行中重复 init 为无操作**（幂等，避免改写 `config_` 与读线程竞争）；重新配置须先 `fini()` 再 `init()`，运行时调级用 `set_level()`（级别是原子量）。`fini()`/静态析构/崩溃抢救结束后日志按丢弃处理（`get_thread_buffer` 见停止标志不再建缓冲）。观测接口：`stat_produce_*`/`stat_consume_*`、`decode_error_count()`（formatter 抛错总数）、`io_drop_bytes()`（输出目的地写失败丢弃字节）、`log_meta_count()`
- `zrlog::literal()` 只可包装**静态存储期**字符串（字面量/全局数组），包装栈局部数组会异步悬垂（C++17 无法编译期强制）；字面量优先用 `"..."_sl`