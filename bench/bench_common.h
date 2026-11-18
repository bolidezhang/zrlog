// 统一对比基准的公共测量框架（Windows / Linux 通用）
//
// 设计：每个被测库编译成独立可执行文件（各库自带 fmt 版本不同，混在一个
// 二进制里会 ODR 冲突），但共用本文件的测量代码，保证口径一致。
//
// 测量要点：
//  - 时间戳用 TSC（__rdtsc），启动时与 steady_clock 校准得到 ns/tick。
//  - 每次迭代取一次时间戳，样本离线排序取百分位（不用每轮均值）。
//  - 延迟测试自旋节流（不用 sleep：睡眠唤醒的 C-state/缓存惩罚会制造
//    微秒级假尾部），保证队列不被打满，测纯前端路径。
//  - 吞吐测试不节流，各库满缓冲策略不同（丢弃/阻塞），如实记录。
//  - 支持 CPU 亲和：环境变量 BENCH_CPUS="0,2,4,6" 指定逻辑核。
//    混合架构（P/E 核）上必须绑到 P 核，否则排名会被 E 核频率拉偏。
//  - 进程优先级拉高（Windows HIGH_PRIORITY_CLASS，不用 REALTIME）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>
static inline uint64_t bench_rdtsc() { return __rdtsc(); }
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t bench_rdtsc() { return __rdtsc(); }
#else
static inline uint64_t bench_rdtsc() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif

namespace bench {

static double g_ns_per_tick = 1.0;
static std::vector<int> g_cpus;  // 空 = 不绑核

inline double calibrate_tsc() {
    const auto wall0 = std::chrono::steady_clock::now();
    const uint64_t t0 = bench_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t t1 = bench_rdtsc();
    const auto wall1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(wall1 - wall0).count();
    const double ticks = static_cast<double>(t1 - t0);
    return ticks > 0 ? ns / ticks : 1.0;
}

// ---- 环境准备：优先级 + 亲和解析 ----
inline void raise_priority() {
#if defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
}

inline void parse_cpus_from_env() {
    const char* s = std::getenv("BENCH_CPUS");
    g_cpus.clear();
    if (!s || !*s) return;
    std::string buf(s);
    size_t pos = 0;
    while (pos < buf.size()) {
        size_t comma = buf.find(',', pos);
        if (comma == std::string::npos) comma = buf.size();
        std::string tok = buf.substr(pos, comma - pos);
        if (!tok.empty()) g_cpus.push_back(std::atoi(tok.c_str()));
        pos = comma + 1;
    }
}

inline void pin_current_thread(int slot) {
    if (g_cpus.empty()) return;
    const int cpu = g_cpus[static_cast<size_t>(slot) % g_cpus.size()];
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

struct Stats {
    double avg_ns = 0, min_ns = 0, p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
};

inline Stats compute_stats(std::vector<uint64_t>& ticks) {
    Stats s;
    if (ticks.empty()) return s;
    std::sort(ticks.begin(), ticks.end());
    const size_t n = ticks.size();
    double sum = 0;
    for (auto t : ticks) sum += static_cast<double>(t);
    auto at = [&](double q) {
        const size_t idx = static_cast<size_t>(q * static_cast<double>(n - 1));
        return static_cast<double>(ticks[idx]) * g_ns_per_tick;
    };
    s.avg_ns  = sum / static_cast<double>(n) * g_ns_per_tick;
    s.min_ns  = static_cast<double>(ticks.front()) * g_ns_per_tick;
    s.max_ns  = static_cast<double>(ticks.back()) * g_ns_per_tick;
    s.p50_ns  = at(0.50);
    s.p90_ns  = at(0.90);
    s.p99_ns  = at(0.99);
    s.p999_ns = at(0.999);
    return s;
}

inline double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Backend {
    std::string name;
    void (*init)(const char* logfile) = nullptr;
    void (*log_one)() = nullptr;
    void (*drain)() = nullptr;
    void (*pre_thread)() = nullptr;
    void (*post_thread)() = nullptr;
    void (*poll)() = nullptr;  // 可选：需用户轮询的库在节流点排空一次
};

constexpr int kLatencyWarmup = 20000;
constexpr int kLatencyIters  = 200000;
constexpr uint64_t kThrottleNs = 20000;

inline void spin_pause_for_ns(uint64_t ns) {
    const uint64_t ticks = static_cast<uint64_t>(static_cast<double>(ns) / g_ns_per_tick);
    const uint64_t start = bench_rdtsc();
    while (bench_rdtsc() - start < ticks) {
#if defined(_WIN32) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }
}

inline Stats run_latency(const Backend& b) {
    pin_current_thread(0);
    if (b.pre_thread) b.pre_thread();
    for (int i = 0; i < kLatencyWarmup; ++i) b.log_one();

    std::vector<uint64_t> ticks;
    ticks.reserve(kLatencyIters);
    for (int i = 0; i < kLatencyIters; ++i) {
        const uint64_t t0 = bench_rdtsc();
        b.log_one();
        const uint64_t t1 = bench_rdtsc();
        ticks.push_back(t1 - t0);
        if ((i & 255) == 0) {
            if (b.poll) b.poll();
            spin_pause_for_ns(kThrottleNs);
        }
    }
    if (b.post_thread) b.post_thread();
    return compute_stats(ticks);
}

struct ThroughputResult {
    double mlogs_per_sec = 0;
    double seconds = 0;
    uint64_t total = 0;
};

inline ThroughputResult run_throughput(const Backend& b, int nthreads, int per_thread) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    ts.reserve(static_cast<size_t>(nthreads));

    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([&, t] {
            pin_current_thread(t);
            if (b.pre_thread) b.pre_thread();
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < per_thread; ++i) b.log_one();
            if (b.post_thread) b.post_thread();
        });
    }
    while (ready.load(std::memory_order_acquire) < nthreads) std::this_thread::yield();

    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    const auto end = std::chrono::steady_clock::now();

    ThroughputResult r;
    r.total = static_cast<uint64_t>(nthreads) * static_cast<uint64_t>(per_thread);
    r.seconds = std::chrono::duration<double>(end - start).count();
    r.mlogs_per_sec = r.seconds > 0 ? static_cast<double>(r.total) / r.seconds / 1e6 : 0;
    return r;
}

inline void print_latency_row(const std::string& name, const Stats& s) {
    std::printf("  %-10s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %9.1f\n",
                name.c_str(), s.avg_ns, s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns,
                s.p999_ns, s.max_ns);
}

// argc/argv: [threads] [per_thread] [mode=lat|thr|both]
inline int run_suite(const Backend& b, int argc, char** argv) {
    const int th_threads = argc > 1 ? std::atoi(argv[1]) : 4;
    const int th_per     = argc > 2 ? std::atoi(argv[2]) : 2000000;
    const std::string mode = argc > 3 ? argv[3] : "both";

    raise_priority();
    parse_cpus_from_env();
    g_ns_per_tick = calibrate_tsc();

    std::string logfile = "bench_" + b.name + ".log";
    std::remove(logfile.c_str());
    b.init(logfile.c_str());

    std::printf("backend=%s TSC=%.6f ns/tick (%.3f GHz) mode=%s cpus=%s\n",
                b.name.c_str(), g_ns_per_tick, 1.0 / g_ns_per_tick, mode.c_str(),
                g_cpus.empty() ? "any" : "(pinned)");

    if (mode == "lat" || mode == "both") {
        const Stats s = run_latency(b);
        std::printf("LATENCY_HEADER avg_ns min_ns p50_ns p90_ns p99_ns p99.9_ns max_ns\n");
        print_latency_row(b.name, s);
    }

    if (mode == "thr" || mode == "both") {
        const ThroughputResult t = run_throughput(b, th_threads, th_per);
        std::printf("THROUGHPUT_HEADER backend mlogs_per_sec seconds total\n");
        std::printf("  %-10s %12.2f %12.3f %10llu\n", b.name.c_str(),
                    t.mlogs_per_sec, t.seconds, static_cast<unsigned long long>(t.total));
    }

    b.drain();
    std::remove(logfile.c_str());
    std::printf("DONE %s\n", b.name.c_str());
    return 0;
}

}  // namespace bench
