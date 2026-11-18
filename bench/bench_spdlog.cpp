// spdlog 后端适配（异步 logger，overrun_oldest 与 zrlog Discard 语义对齐）
#include "bench_common.h"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace {

std::shared_ptr<spdlog::async_logger> g_logger;

void init(const char* logfile) {
    std::remove(logfile);
    spdlog::init_thread_pool(1 << 20, 1);
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logfile, true);
    g_logger = std::make_shared<spdlog::async_logger>(
        "bench", sink, spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
    g_logger->set_level(spdlog::level::info);
    g_logger->set_pattern("%v");
}

void log_one() {
    g_logger->info("Hello World, the answer is {} and {:.2f}", 42, 3.14);
}

void drain() {
    g_logger.reset();
    spdlog::shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    bench::Backend b;
    b.name = "spdlog";
    b.init = &init;
    b.log_one = &log_one;
    b.drain = &drain;
    return bench::run_suite(b, argc, argv);
}
