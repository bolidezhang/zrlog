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

    CHECK_EQ(static_cast<uint32_t>(ZRLOG_VERSION), 20405u);
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
