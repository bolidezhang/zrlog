// 毒丸消息（poison pill）回归测试。
//
// 背景：consumers 曾在 decoder 之后才推进游标，若 decoder 抛出非
// fmt::format_error 的异常（bad_alloc、自定义 formatter 抛错等），异常会
// 逃逸到 poll_routine 顶层 catch，游标不前进，下一轮又读到同一条坏日志，
// 形成死循环——后台线程空转、后续日志全部堵死，连 fini() 都永久挂起。
// 实测曾出现 79,804 次重复异常且进程无法退出。
//
// 本测试要求：
//   1) 投毒那条日志被吞掉（输出 [FMT_ERROR] 或直接丢弃），不重试
//   2) 后续日志仍能正常落盘
//   3) fini() 能正常返回（不挂死）
#include "zrlog.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

// 抛非 fmt::format_error 的 formatter：模拟 OOM / 自定义异常
struct PoisonArg {};
template <>
struct fmt::formatter<PoisonArg> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const PoisonArg&, Ctx& ctx) const -> decltype(ctx.out()) {
        throw std::bad_alloc();
    }
};

// 只抛 fmt::format_error 的 formatter：验证原有分支仍工作
struct BadFormatArg {};
template <>
struct fmt::formatter<BadFormatArg> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const BadFormatArg&, Ctx& ctx) const -> decltype(ctx.out()) {
        throw fmt::format_error("simulated format error");
    }
};

static bool file_contains(const char* path, const char* needle) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return false;
    std::stringstream ss; ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

int main() {
    const char* path = "poison_test.log";
    std::remove(path);

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = path;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.force_flush_interval_ms = 50;
    cfg.idle_wait_interval_us = 100;
    ZRLOG_INIT_CONF(cfg);

    ZRLOG_INFO("before_poison {}", 1);

    // 投毒 1：非 format_error 异常
    ZRLOG_DYN_INFO(std::string("poison_alloc {}"), PoisonArg{});
    // 投毒 2：format_error 异常
    ZRLOG_DYN_INFO(std::string("poison_format {}"), BadFormatArg{});

    // 后续正常日志必须全部落盘
    for (int i = 0; i < 100; ++i) {
        ZRLOG_INFO("after_poison {}", i);
    }

    // fini() 必须能返回（旧实现会在此永久挂起）
    auto t0 = std::chrono::steady_clock::now();
    ZRLOG_FINI();
    const double fini_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    const bool has_before = file_contains(path, "before_poison 1");
    const bool has_after  = file_contains(path, "after_poison 99");

    std::printf("fini_ms=%.0f before=%d after=%d\n", fini_ms, has_before, has_after);

    std::remove(path);

    if (!has_before || !has_after) {
        std::printf("RESULT: FAIL (poison pill blocked the consumer)\n");
        return 1;
    }
    if (fini_ms > 10000.0) {
        std::printf("RESULT: FAIL (fini took %.0f ms, likely stuck)\n", fini_ms);
        return 1;
    }
    std::printf("RESULT: PASS (poison swallowed, later logs intact, fini returned)\n");
    return 0;
}
