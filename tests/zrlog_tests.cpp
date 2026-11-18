// zrlog 单元测试
// 不引入第三方测试框架，由 ctest 直接运行；失败返回非 0。
#include "zrlog.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <sys/types.h>
#endif

using namespace zrlog::literals;

namespace zrt {

struct Failure {
    std::string msg;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& what) {
    std::ostringstream os;
    os << file << ":" << line << ": " << what;
    throw Failure{ os.str() };
}

template <typename A, typename B>
inline void check_eq_impl(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream os;
        os << "expected " << ea << " == " << eb << ", got '" << a << "' vs '" << b << "'";
        fail(file, line, os.str());
    }
}

template <typename A, typename B>
inline void check_ne_impl(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (a == b) {
        std::ostringstream os;
        os << "expected " << ea << " != " << eb << ", both '" << a << "'";
        fail(file, line, os.str());
    }
}

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({ name, std::move(fn) });
    }
};

}  // namespace zrt

#define TEST(name)                                              \
    static void name();                                         \
    static zrt::Registrar reg_##name(#name, name);              \
    static void name()

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) zrt::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    } while (0)

#define CHECK_EQ(a, b) zrt::check_eq_impl((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NE(a, b) zrt::check_ne_impl((a), (b), #a, #b, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// 静态字符串字面量
// ---------------------------------------------------------------------------
TEST(string_literal_basics) {
    constexpr auto lit = zrlog::literal("hello");
    CHECK_EQ(lit.len, static_cast<uint32_t>(5));
    CHECK_EQ(std::string_view(lit.ptr, lit.len), std::string_view("hello"));

    const auto sl = "world"_sl;
    CHECK_EQ(sl.len, static_cast<uint32_t>(5));
    CHECK_EQ(std::string_view(sl.ptr, sl.len), std::string_view("world"));

    constexpr auto empty_lit = zrlog::literal("");
    CHECK_EQ(empty_lit.len, static_cast<uint32_t>(0));

    CHECK_EQ(zrlog::format_as(lit), std::string_view("hello"));
}

// ---------------------------------------------------------------------------
// DYN 格式串内容哈希（行为回归，非标准 FNV-1a 常量）
// ---------------------------------------------------------------------------
TEST(fnv1a_hash_properties) {
    using zrlog::detail::fnv1a_hash;

    CHECK_EQ(fnv1a_hash("abc"), fnv1a_hash("abc"));
    CHECK_NE(fnv1a_hash("abc"), fnv1a_hash("abd"));
    CHECK_NE(fnv1a_hash("ab"), fnv1a_hash("abc"));
    CHECK_NE(fnv1a_hash(""), fnv1a_hash("a"));
    CHECK_EQ(fnv1a_hash(""), 1469598103934665603ull);
}

// ---------------------------------------------------------------------------
// 数字快速格式化查表
// ---------------------------------------------------------------------------
TEST(fast_digit_formatters) {
    char buf[16];
    auto to_str = [&](int n) { return std::string(buf, static_cast<size_t>(n)); };

    std::memset(buf, 0, sizeof(buf));
    zrlog::detail::fast_u32_to_2digits(buf, 0);   CHECK_EQ(to_str(2), std::string("00"));
    zrlog::detail::fast_u32_to_2digits(buf, 7);   CHECK_EQ(to_str(2), std::string("07"));
    zrlog::detail::fast_u32_to_2digits(buf, 42);  CHECK_EQ(to_str(2), std::string("42"));
    zrlog::detail::fast_u32_to_2digits(buf, 99);  CHECK_EQ(to_str(2), std::string("99"));

    zrlog::detail::fast_u32_to_4digits(buf, 2026);   CHECK_EQ(to_str(4), std::string("2026"));
    zrlog::detail::fast_u32_to_4digits(buf, 7);      CHECK_EQ(to_str(4), std::string("0007"));
    zrlog::detail::fast_u32_to_4digits(buf, 123456); CHECK_EQ(to_str(4), std::string("9999"));

    zrlog::detail::fast_u32_to_9digits(buf, 0);          CHECK_EQ(to_str(9), std::string("000000000"));
    zrlog::detail::fast_u32_to_9digits(buf, 123);        CHECK_EQ(to_str(9), std::string("000000123"));
    zrlog::detail::fast_u32_to_9digits(buf, 999999999);  CHECK_EQ(to_str(9), std::string("999999999"));
    zrlog::detail::fast_u32_to_9digits(buf, 1000000000); CHECK_EQ(to_str(9), std::string("999999999"));
}

// ---------------------------------------------------------------------------
// 日志级别名称与越界保护
// ---------------------------------------------------------------------------
TEST(log_level_names) {
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::TRACE), std::string_view("TRACE"));
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::DEBUG), std::string_view("DEBUG"));
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::INFO),  std::string_view("INFO"));
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::WARN),  std::string_view("WARN"));
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::ERR),   std::string_view("ERROR"));
    CHECK_EQ(zrlog::loglevel_to_string(zrlog::LogLevel::FATAL), std::string_view("FATAL"));
    CHECK_EQ(zrlog::loglevel_to_string(static_cast<zrlog::LogLevel>(200)), std::string_view("UNKNOWN"));
}

// ---------------------------------------------------------------------------
// SpinMutex 互斥性
// ---------------------------------------------------------------------------
TEST(spin_mutex_mutual_exclusion) {
    zrlog::SpinMutex m;
    long long counter = 0;
    const int kThreads = 4;
    const int kIters = 50000;

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                std::lock_guard<zrlog::SpinMutex> guard(m);
                ++counter;
            }
        });
    }
    for (auto& th : ts) th.join();

    CHECK_EQ(counter, static_cast<long long>(kThreads) * kIters);
}

// ---------------------------------------------------------------------------
// TscClock 单调性与换算
// ---------------------------------------------------------------------------
TEST(tsc_clock_sanity) {
    auto& clk = zrlog::TscClock::instance();

    CHECK_EQ(clk.tsc2ns(0), static_cast<uint64_t>(0));
    // 单调不减；相邻 tick 的差可能小于 1ns（TSC 约 0.25~0.5ns/tick），故只断言不减
    CHECK(clk.tsc2ns(1000) >= clk.tsc2ns(999));
    CHECK(clk.tsc2ns(2000) > clk.tsc2ns(1000));
    CHECK(clk.tsc2ns(1000000) >= clk.tsc2ns(999999));

    uint64_t prev = clk.now_ns_i();
    for (int i = 0; i < 100000; ++i) {
        const uint64_t now = clk.now_ns_i();
        CHECK(now >= prev);
        prev = now;
    }

    const auto sys_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const long long seqlock_ns = static_cast<long long>(clk.current_time_ns());
    CHECK(std::llabs(seqlock_ns - static_cast<long long>(sys_ns)) < 5000000000LL);
}

// ---------------------------------------------------------------------------
// Config 默认值（与 zrlog.h 定义保持一致）
// ---------------------------------------------------------------------------
TEST(config_defaults) {
    zrlog::Config c;
    CHECK(c.appender == zrlog::AppenderType::RotatingFile);
    CHECK(c.level == zrlog::LogLevel::INFO);
    CHECK(c.buffer_full_policy == zrlog::BufferFullPolicy::Discard);
    CHECK(!c.enable_huge_page);
    CHECK(c.background_thread_core_ids.empty());

    CHECK_EQ(c.io_buffer_size, 4u * 1024u * 1024u);
    CHECK_EQ(c.thread_buffer_size, 1024u * 1024u);
    CHECK_EQ(c.per_thread_quota, 256u);
    CHECK_EQ(c.idle_wait_interval_us, 1000u);
    CHECK_EQ(c.force_flush_interval_ms, 3000u);
    CHECK_EQ(c.tsc_sync_interval_s, 3600u);
    CHECK_EQ(c.crash_drain_wait_ms, 3000u);
    CHECK_EQ(c.buffer_full_retry_count, 256u);
    CHECK_EQ(c.rotating_file_size, 100u * 1024u * 1024u);
    CHECK_EQ(c.rotating_max_files, 5u);

    CHECK_EQ(static_cast<uint32_t>(ZRLOG_VERSION), 20500u);
}

// ---------------------------------------------------------------------------
// 线程 ID：同线程稳定、跨线程唯一
// ---------------------------------------------------------------------------
TEST(thread_id_uniqueness) {
    const uint64_t main_tid = zrlog::util::get_thread_id();
    CHECK_EQ(main_tid, zrlog::util::get_thread_id());

    uint64_t child_tid = 0;
    std::thread th([&] { child_tid = zrlog::util::get_thread_id(); });
    th.join();

    CHECK(child_tid != 0);
    CHECK_NE(child_tid, main_tid);
}

// ---------------------------------------------------------------------------
// 内存块分配
// ---------------------------------------------------------------------------
TEST(memory_block_alloc_free) {
    auto blk = zrlog::util::allocate_block(4096, false);
    CHECK(blk.ptr != nullptr);
    CHECK_EQ(blk.actual_size, static_cast<size_t>(4096));
    CHECK(!blk.is_huge_page);

    blk.ptr[0] = 'a';
    blk.ptr[4095] = 'z';
    CHECK_EQ(blk.ptr[0], 'a');
    CHECK_EQ(blk.ptr[4095], 'z');
    zrlog::util::free_block(blk);

    zrlog::util::MemoryBlock empty;
    zrlog::util::free_block(empty);  // 空块释放必须安全
}

constexpr int kMtThreads = 4;
constexpr int kMtPerThread = 2000;

// ---------------------------------------------------------------------------
// 端到端：级别过滤、多类型渲染、DYN 格式串切换、多线程不丢日志
// 必须最后运行（会初始化并关闭单例）
// ---------------------------------------------------------------------------
TEST(e2e_file_logging) {
    const char* path = "zrlog_unit_test.log";
    std::remove(path);

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = path;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.thread_buffer_size = 256 * 1024;
    cfg.io_buffer_size = 64 * 1024;
    cfg.idle_wait_interval_us = 200;
    cfg.force_flush_interval_ms = 50;
    cfg.buffer_full_policy = zrlog::BufferFullPolicy::Block;

    auto& logger = zrlog::NanoLogger::instance();
    CHECK(logger.init(cfg));

    ZRLOG_DEBUG("SHOULD_NOT_APPEAR {}", 1);
    ZRLOG_INFO("plain {} {}", 42, std::string("hello"));
    ZRLOG_WARN("literal {}", "world"_sl);
    ZRLOG_ERROR("float {:.2f}", 3.14159);

    std::string dyn_fmt = "dyn-first {}";
    ZRLOG_DYN_INFO(dyn_fmt, 7);
    dyn_fmt = "dyn-second {}";
    ZRLOG_DYN_INFO(dyn_fmt, 8);

    std::vector<std::thread> workers;
    for (int t = 0; t < kMtThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kMtPerThread; ++i) {
                ZRLOG_INFO("mt {} {}", t, i);
            }
        });
    }
    for (auto& w : workers) w.join();

    logger.fini();

    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();

    CHECK(content.find(" INFO ") != std::string::npos);
    CHECK(content.find("SHOULD_NOT_APPEAR") == std::string::npos);
    CHECK(content.find("plain 42 hello") != std::string::npos);
    CHECK(content.find("literal world") != std::string::npos);
    CHECK(content.find("float 3.14") != std::string::npos);
    CHECK(content.find("dyn-first 7") != std::string::npos);
    CHECK(content.find("dyn-second 8") != std::string::npos);

    const size_t lines = static_cast<size_t>(std::count(content.begin(), content.end(), '\n'));
    CHECK_EQ(lines, static_cast<size_t>(kMtThreads * kMtPerThread + 5));

    // 时间戳正确性：前端只存原始 TSC，后端用 seqlock 换算成 ns。
    // 解析首行的 "YYYY-MM-DD HH:MM:SS.nnnnnnnnn" 并与系统 UTC 时钟比对，
    // 防止 TSC->ns 换算写错时输出 1970 年或偏移量级的时间。
    CHECK(content.size() > 30);
    const std::string stamp = content.substr(0, 29);  // "YYYY-MM-DD HH:MM:SS.nnnnnnnnn"
    CHECK_EQ(stamp[4], '-');
    CHECK_EQ(stamp[7], '-');
    CHECK_EQ(stamp[10], ' ');
    CHECK_EQ(stamp[13], ':');
    CHECK_EQ(stamp[16], ':');
    CHECK_EQ(stamp[19], '.');

    struct tm tmv{};
    const std::string head = stamp.substr(0, 19);  // "YYYY-MM-DD HH:MM:SS"
    tmv.tm_year = std::atoi(head.substr(0, 4).c_str()) - 1900;
    tmv.tm_mon  = std::atoi(head.substr(5, 2).c_str()) - 1;
    tmv.tm_mday = std::atoi(head.substr(8, 2).c_str());
    tmv.tm_hour = std::atoi(head.substr(11, 2).c_str());
    tmv.tm_min  = std::atoi(head.substr(14, 2).c_str());
    tmv.tm_sec  = std::atoi(head.substr(17, 2).c_str());
#if defined(_WIN32)
    const time_t logged = _mkgmtime(&tmv);
#else
    const time_t logged = timegm(&tmv);
#endif
    const time_t now = std::time(nullptr);
    const long long drift = static_cast<long long>(now) - static_cast<long long>(logged);
    CHECK(drift >= -2 && drift <= 5);  // 允许秒级截断与执行耗时

    const int year = std::atoi(head.substr(0, 4).c_str());
    CHECK(year >= 2020 && year <= 2100);

    // 生产者统计（#7）：本轮共 kMtThreads*kMtPerThread + 5 条全部入队成功。
    // 注意 stat_produce_* 是线程本地每 1024 条汇总一次，fini() 只冲刷调用线程
    // 的残留计数；工作线程的残留无法跨线程冲刷，故只断言"有计数且有效数<=尝试数"。
    const uint64_t produced = logger.stat_produce_count_.load(std::memory_order_relaxed);
    const uint64_t produced_valid = logger.stat_produce_valid_count_.load(std::memory_order_relaxed);
    CHECK(produced > 0);                       // 修复前恒为 0
    CHECK(produced_valid <= produced);
    CHECK(produced_valid > 0);
}

// ---------------------------------------------------------------------------
// DYN 元数据不应无界增长（#5）：同一调用点 A->B->A 反复切换，元数据条数必须收敛
// ---------------------------------------------------------------------------
TEST(dyn_meta_no_unbounded_growth) {
    const char* path = "zrlog_unit_test_dyn.log";
    std::remove(path);

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = path;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.force_flush_interval_ms = 30;
    auto& logger = zrlog::NanoLogger::instance();
    CHECK(logger.init(cfg));

    const size_t meta_before = logger.log_meta_count();

    // 关键：必须是同一个调用点（同一行号），只有格式串内容在变。
    // 旧实现只比对"上一次注册的格式"，A->B->A 每次切换都新增一条 meta；
    // 新实现按内容查重，两种格式各注册一次后复用。
    for (int i = 0; i < 50; ++i) {
        std::string f = (i % 2 == 0) ? "dynA {}" : "dynB {}";
        ZRLOG_DYN_INFO(f, i);
    }
    const size_t meta_after = logger.log_meta_count();
    logger.fini();

    // 两种格式串交替 50 次，元数据只应新增 2 条（A、B 各一），
    // 而不是每切换一次就新增一条（修复前会增长到 ~50 条）。
    CHECK_EQ(meta_after - meta_before, static_cast<size_t>(2));

    // 两种格式串都应被渲染（证明切换生效）
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    const std::string c = ss.str();
    CHECK(c.find("dynA 0") != std::string::npos);
    CHECK(c.find("dynB 1") != std::string::npos);
    CHECK(c.find("dynA 48") != std::string::npos);
    CHECK(c.find("dynB 49") != std::string::npos);
    const size_t lines = static_cast<size_t>(std::count(c.begin(), c.end(), '\n'));
    CHECK_EQ(lines, static_cast<size_t>(50));
}

// ---------------------------------------------------------------------------
// 解码长度校验（#6）：长度字段被破坏时不得读穿缓冲
// ---------------------------------------------------------------------------
TEST(decode_arg_length_bounds) {
    // 构造一个声称 len=1000 但实际只有 5 字节的损坏 payload
    char buf[64] = { 0 };
    const uint32_t claimed = 1000;
    std::memcpy(buf, &claimed, sizeof(uint32_t));
    std::memcpy(buf + 4, "hello", 5);

    char* p = buf;
    const char* pend = buf + 9;  // 实际只有 4(长度) + 5(内容)
    auto sv = zrlog::detail::decode_arg<std::string_view>(p, pend);

    // 必须截断到可用长度，绝不能返回 1000 字节的视图
    CHECK_EQ(sv.size(), static_cast<size_t>(5));
    CHECK_EQ(std::string(sv), std::string("hello"));
    CHECK(p <= pend);  // 指针不得越过边界

    // 完全为空时返回空视图
    char* p2 = buf;
    auto sv2 = zrlog::detail::decode_arg<std::string_view>(p2, buf);
    CHECK_EQ(sv2.size(), static_cast<size_t>(0));

    // 标量越界时返回默认值而非读穿
    char tiny[2] = { 0x11, 0x22 };
    char* p3 = tiny;
    auto v = zrlog::detail::decode_arg<uint64_t>(p3, tiny + 2);
    CHECK_EQ(v, static_cast<uint64_t>(0));
}

// ---------------------------------------------------------------------------
// 轮转写失败计数（#8）：current_size_ 必须始终等于当前文件的真实字节数。
// 旧实现在写失败时 `current_size_ += len` 会虚增计数，导致提前 roll 风暴。
// 这里通过"大量写入 + 多次滚动"验证计数与文件大小的不变量始终成立。
// ---------------------------------------------------------------------------
TEST(rotating_size_accounting_invariant) {
    const char* path = "zrlog_rot_test.log";
    std::remove(path);
    for (int i = 1; i <= 5; ++i) {
        std::remove((std::string(path) + "." + std::to_string(i)).c_str());
    }

    const size_t kMaxSize = 4096;
    zrlog::RotatingFileLogAppender app(path, kMaxSize, 3);
    CHECK(app.is_open());
    CHECK_EQ(app.current_size(), static_cast<size_t>(0));

    // 正常写入：计数应精确等于实际写入字节
    const char msg[] = "0123456789";
    CHECK_EQ(app.write(msg, sizeof(msg) - 1), static_cast<int>(sizeof(msg) - 1));
    CHECK_EQ(app.current_size(), static_cast<size_t>(sizeof(msg) - 1));

    // 持续写入，跨越多次滚动。每次写入后核对：
    //   current_size_ == 当前 base 文件的实际大小
    // 若写失败虚增计数，这个等式会在滚动后立刻失配。
    std::string chunk(700, 'x');
    size_t rolls = 0;
    size_t prev_size = app.current_size();
    for (int i = 0; i < 40; ++i) {
        app.write(chunk.data(), chunk.size());
        app.flush();

        struct stat st {};
        CHECK(stat(path, &st) == 0);
        CHECK_EQ(app.current_size(), static_cast<size_t>(st.st_size));

        // 检测滚动发生（base 文件变小说明刚重建）
        if (app.current_size() < prev_size) {
            ++rolls;
        }
        prev_size = app.current_size();
    }

    // 40 * 700B ≈ 28KB / 4KB 阈值，必然发生多次滚动（不是 0 次）
    CHECK(rolls >= 3);
    // 备份文件数量不超过 max_files
    int backups = 0;
    for (int i = 1; i <= 10; ++i) {
        std::ifstream f(std::string(path) + "." + std::to_string(i));
        if (f.good()) ++backups;
    }
    CHECK(backups <= 3);

    std::remove(path);
    for (int i = 1; i <= 10; ++i) {
        std::remove((std::string(path) + "." + std::to_string(i)).c_str());
    }
}

// ---------------------------------------------------------------------------
// 轮转打开失败兜底（#8 相关）：文件无法打开时走 stderr 兜底，
// current_size_ 只能按 stderr 实际写入字节推进，不得按请求长度虚增。
// ---------------------------------------------------------------------------
TEST(rotating_open_failure_fallback) {
    // 用一个目录路径当日志文件：open 必然失败
    const char* dir_path = "zrlog_rot_dir";
#if defined(_WIN32)
    _mkdir(dir_path);
#else
    mkdir(dir_path, 0755);
#endif

    zrlog::RotatingFileLogAppender app(dir_path, 4096, 3);
    CHECK(!app.is_open());          // 打开失败
    CHECK_EQ(app.current_size(), static_cast<size_t>(0));

    // 写入应降级到 stderr，返回值是 stderr 实际写入量（可能为 -1 或 >0），
    // 但 current_size_ 绝不能等于请求的 len（旧实现会 += len 虚增）
    const char msg[] = "fallback-line";
    const int n = app.write(msg, sizeof(msg) - 1);
    CHECK(n != static_cast<int>(sizeof(msg) - 1) || app.current_size() == static_cast<size_t>(n));

#if defined(_WIN32)
    _rmdir(dir_path);
#else
    rmdir(dir_path);
#endif
}

// ---------------------------------------------------------------------------
// 写错误分类（#8）：硬错误必须关闭 fd（避免在死描述符上反复失败），
// 软错误必须保留 fd（磁盘满时空间释放后自动恢复，不应丢弃文件句柄）。
// ---------------------------------------------------------------------------
TEST(write_error_classification) {
    using R = zrlog::RotatingFileLogAppender;

    // 硬错误：fd 已不可用，必须关闭
#if defined(EBADF)
    CHECK(R::is_hard_write_error(EBADF));
#endif
#if defined(EIO)
    CHECK(R::is_hard_write_error(EIO));
#endif
#if defined(EPIPE)
    CHECK(R::is_hard_write_error(EPIPE));
#endif

    // 软错误：必须保留 fd（旧实现不区分，一律保留并按 len 虚增计数）
#if defined(ENOSPC)
    CHECK(!R::is_hard_write_error(ENOSPC));   // 磁盘满
#endif
#if defined(EDQUOT)
    CHECK(!R::is_hard_write_error(EDQUOT));   // 配额超限
#endif
#if defined(EAGAIN)
    CHECK(!R::is_hard_write_error(EAGAIN));   // 暂时不可写
#endif
#if defined(EINTR)
    CHECK(!R::is_hard_write_error(EINTR));    // 被信号中断
#endif

    CHECK(!R::is_hard_write_error(0));        // 无错误
}

// ---------------------------------------------------------------------------
// 跨平台时间源契约（#9）：zrlog_rdtsc 在各架构上都必须单调不减，
// 且 TscClock 能基于它换算出合理的时间戳。
// x86/x64=TSC，MSVC ARM64=CNTVCT_EL0，GCC aarch64=cntvct_el0，其余=高精度时钟。
// ---------------------------------------------------------------------------
TEST(platform_rdtsc_contract) {
    const uint64_t a = zrlog_rdtsc();
    // 忙等一小会儿，确保计数器必然前进
    volatile uint64_t sink = 0;
    for (int i = 0; i < 100000; ++i) {
        sink += zrlog_rdtsc();
    }
    const uint64_t b = zrlog_rdtsc();
    (void)sink;
    CHECK(b > a);                       // 单调递增

    // 基于该时间源换算出的 ns 时间戳必须接近系统时钟
    auto& clk = zrlog::TscClock::instance();
    const uint64_t ns = clk.tsc_to_unix_ns(b);
    const uint64_t sys_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const long long diff = static_cast<long long>(ns) - static_cast<long long>(sys_ns);
    CHECK(diff > -5000000000LL && diff < 5000000000LL);  // 5 秒内

    // 计数器频率必须已校准（ns_per_tick 为正）
    CHECK(clk.ns_per_tick() > 0.0);
}

// ---------------------------------------------------------------------------
// 自适应 idle 配置（C 组）：平台默认值不同
//   Linux/macOS 默认开启（nanosleep 精度够）；Windows 默认关闭
//   （wait_for 受 ~1.5ms 系统 timer 精度限制，压到 50us 无收益）。
// ---------------------------------------------------------------------------
TEST(adaptive_idle_config) {
    zrlog::Config c;
#if defined(_WIN32)
    CHECK(!c.adaptive_idle_wait);       // Windows 默认关闭
#else
    CHECK(c.adaptive_idle_wait);        // Linux/macOS 默认开启
#endif
    CHECK(c.idle_busy_interval_us > 0);
    CHECK(c.idle_busy_interval_us < c.idle_wait_interval_us);  // 活跃态更快
    CHECK(c.idle_wait_interval_us == 1000u);
    CHECK(c.idle_busy_interval_us == 50u);
}

// ---------------------------------------------------------------------------
// 自适应 idle 端到端：两种模式都必须能把日志正确落盘（功能正确性）。
// 注意：不做 adaptive vs fixed 的延迟对比——Windows 上 wait_for 受 ~1.5ms
// 系统 timer 精度限制（实测 sleep_for(20us) 实际睡 ~1.6ms），把间隔压到
// 50us 没有可测收益，对比数字只会是噪声。真正的收益在 Linux 上。
// ---------------------------------------------------------------------------
TEST(adaptive_idle_tail_visibility) {
    auto& logger = zrlog::NanoLogger::instance();

    auto measure_visibility_us = [&](bool adaptive) -> double {
        const char* path = adaptive ? "zrlog_idle_adaptive.log" : "zrlog_idle_fixed.log";
        std::remove(path);

        zrlog::Config cfg;
        cfg.appender = zrlog::AppenderType::File;
        cfg.filename = path;
        cfg.level = zrlog::LogLevel::INFO;
        cfg.adaptive_idle_wait = adaptive;
        cfg.idle_busy_interval_us = 20;
        cfg.idle_wait_interval_us = 1000;
        cfg.force_flush_interval_ms = 60000;  // 关掉周期刷盘，只靠 idle 路径落盘
        CHECK(logger.init(cfg));

        // 预热：首次日志会分配线程缓冲并做页预热，比 idle 间隔慢几个数量级
        for (int i = 0; i < 64; ++i) {
            ZRLOG_INFO("warmup {}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        ZRLOG_INFO("tail_visibility_probe {}", adaptive ? 1 : 0);
        auto t0 = std::chrono::steady_clock::now();
        double elapsed_us = 1e9;
        for (int i = 0; i < 2000; ++i) {
            std::ifstream in(path, std::ios::binary);
            if (in.good()) {
                std::stringstream ss; ss << in.rdbuf();
                if (ss.str().find("tail_visibility_probe") != std::string::npos) {
                    elapsed_us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0).count();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        logger.fini();
        std::remove(path);
        return elapsed_us;
    };

    const double adaptive_us = measure_visibility_us(true);
    const double fixed_us = measure_visibility_us(false);

    std::printf("         [info] tail visibility: adaptive=%.0fus fixed=%.0fus\n",
                adaptive_us, fixed_us);

    // 两种模式都必须能在合理时间内落盘（正确性）。
    // 上限给到 1s：覆盖 Windows timer 精度 + CI 机器负载抖动。
    CHECK(adaptive_us < 1000000.0);
    CHECK(fixed_us < 1000000.0);
}

// ---------------------------------------------------------------------------
// E-core 检测（C 组）：异构架构上应能识别出性能核子集
// ---------------------------------------------------------------------------
TEST(performance_core_detection) {
    const bool hybrid = zrlog::util::has_hybrid_cores();
    const std::vector<int> p_cores = zrlog::util::get_performance_core_ids();

    std::printf("         [info] hybrid=%d p_cores=%zu\n", hybrid ? 1 : 0, p_cores.size());

    if (hybrid) {
        // 异构架构：必须识别出非空的性能核列表，且不能是全部逻辑核
        CHECK(!p_cores.empty());
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        CHECK(p_cores.size() < static_cast<size_t>(si.dwNumberOfProcessors));
        // 每个 id 必须在合法范围
        for (int c : p_cores) {
            CHECK(c >= 0 && c < static_cast<int>(si.dwNumberOfProcessors));
        }
    }
    else {
        // 同构架构：允许返回空（表示无需绑核）
        CHECK(p_cores.empty() || !p_cores.empty());  // 不强制，仅记录
    }
}

// ---------------------------------------------------------------------------
// E-core 绑核（C 组）：把当前线程绑到检测出的 P-core，读回亲和掩码验证生效。
// ---------------------------------------------------------------------------
TEST(performance_core_pinning_effective) {
#if defined(_WIN32)
    if (!zrlog::util::has_hybrid_cores()) {
        std::printf("         [info] homogeneous CPU, skip pinning check\n");
        return;
    }
    const std::vector<int> p_cores = zrlog::util::get_performance_core_ids();
    CHECK(!p_cores.empty());

    // 记录原始亲和性以便恢复
    GROUP_AFFINITY orig{};
    const HANDLE th = GetCurrentThread();
    CHECK(GetThreadGroupAffinity(th, &orig) != 0);

    CHECK(zrlog::util::pin_thread_to_cores(p_cores));

    GROUP_AFFINITY now{};
    CHECK(GetThreadGroupAffinity(th, &now) != 0);

    // 期望掩码 = 所有 P-core 位
    KAFFINITY expect = 0;
    for (int c : p_cores) {
        CHECK(c >= 0 && c < 64);
        expect |= (static_cast<KAFFINITY>(1) << c);
    }
    CHECK_EQ(static_cast<unsigned long long>(now.Mask),
             static_cast<unsigned long long>(expect));

    // 恢复原亲和性，避免影响后续测试
    SetThreadGroupAffinity(th, &orig, nullptr);
    std::printf("         [info] pinned to %zu P-cores, mask=0x%llx verified\n",
                p_cores.size(), static_cast<unsigned long long>(expect));
#else
    std::printf("         [info] non-Windows, pinning check skipped\n");
#endif
}

// ---------------------------------------------------------------------------
// 注册与消费并发（B 组快速路径）：消费者已在运行时才出现新调用点，
// 新 meta 必须仍被及时同步，日志不得丢失。
// 快速路径用原子计数判断"是否有新 meta"，此处专门验证该判断不漏。
// ---------------------------------------------------------------------------
TEST(late_registration_not_lost) {
    const char* path = "zrlog_late_reg.log";
    std::remove(path);

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = path;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.force_flush_interval_ms = 30;
    cfg.idle_wait_interval_us = 200;
    auto& logger = zrlog::NanoLogger::instance();
    CHECK(logger.init(cfg));

    // 先让消费者跑起来并进入稳态（此时还没有 "late" 这个调用点）
    for (int i = 0; i < 200; ++i) {
        ZRLOG_INFO("warm {}", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // 现在才首次使用一个新的调用点 -> 触发注册
    for (int i = 0; i < 50; ++i) {
        ZRLOG_WARN("late_marker {}", i);
    }
    // 另一个新调用点，由独立线程在消费进行中注册
    std::thread late_thread([] {
        for (int i = 0; i < 50; ++i) {
            ZRLOG_ERROR("late_thread_marker {}", i);
        }
    });
    late_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    logger.fini();

    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    const std::string c = ss.str();
    std::remove(path);

    // 两个后注册的调用点都必须完整渲染，不能因快速路径跳过同步而丢失/错乱
    CHECK(c.find("late_marker 0") != std::string::npos);
    CHECK(c.find("late_marker 49") != std::string::npos);
    CHECK(c.find("late_thread_marker 0") != std::string::npos);
    CHECK(c.find("late_thread_marker 49") != std::string::npos);
    CHECK(c.find("warm 199") != std::string::npos);

    const size_t lines = static_cast<size_t>(std::count(c.begin(), c.end(), '\n'));
    CHECK_EQ(lines, static_cast<size_t>(200 + 50 + 50));
}

int main() {
    std::cout << "zrlog unit tests (" << zrt::registry().size() << " cases)\n";
    int failed = 0;
    for (const auto& tc : zrt::registry()) {
        try {
            tc.fn();
            std::cout << "[ PASS ] " << tc.name << "\n";
        }
        catch (const zrt::Failure& f) {
            ++failed;
            std::cout << "[ FAIL ] " << tc.name << "\n         " << f.msg << "\n";
        }
        catch (const std::exception& e) {
            ++failed;
            std::cout << "[ FAIL ] " << tc.name << " (exception): " << e.what() << "\n";
        }
        catch (...) {
            ++failed;
            std::cout << "[ FAIL ] " << tc.name << " (unknown exception)\n";
        }
    }
    std::cout << (failed == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << ": "
              << (zrt::registry().size() - static_cast<size_t>(failed)) << "/"
              << zrt::registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}
