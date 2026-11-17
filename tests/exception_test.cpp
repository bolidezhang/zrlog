// 异常屏障验证：自定义 formatter 抛异常时，后台消费线程必须不挂掉，
// 而是输出一行 [FMT_ERROR]，且进程正常退出。
#include "zrlog.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

struct Throwing {};

template <>
struct fmt::formatter<Throwing> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    template <typename Ctx>
    auto format(const Throwing&, Ctx& ctx) const -> decltype(ctx.out()) {
        throw std::runtime_error("boom from formatter");
    }
};

int main() {
    const char* path = "exc_test.log";
    std::remove(path);

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = path;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.force_flush_interval_ms = 50;
    ZRLOG_INIT_CONF(cfg);

    ZRLOG_INFO("before {}", 1);
    // 静态路径：编译期格式串，但 formatter 运行时抛
    ZRLOG_INFO("danger {}", Throwing{});
    ZRLOG_INFO("after {}", 2);
    ZRLOG_FINI();

    std::ifstream in(path, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    const std::string c = ss.str();

    const bool has_before = c.find("before 1") != std::string::npos;
    const bool has_after = c.find("after 2") != std::string::npos;
    const bool has_err = c.find("FMT_ERROR") != std::string::npos;

    std::printf("before=%d after=%d fmt_error=%d\n", has_before, has_after, has_err);
    // 关键：进程没崩（能走到这里），且崩溃那条被兜底，前后日志都在
    const bool ok = has_before && has_after && has_err;
    std::printf("%s\n", ok ? "RESULT: PASS (exception contained, process alive)" : "RESULT: FAIL");
    return ok ? 0 : 1;
}
