// zrlog 后端适配
#include "bench_common.h"
#include "zrlog.h"

namespace {

zrlog::NanoLogger* g_logger = nullptr;

void init(const char* logfile) {
    std::remove(logfile);
    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = logfile;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.thread_buffer_size = 8 * 1024 * 1024;
    cfg.io_buffer_size = 8 * 1024 * 1024;
    cfg.buffer_full_policy = zrlog::BufferFullPolicy::Discard;
    g_logger = &zrlog::NanoLogger::instance();
    g_logger->init(cfg);
}

void log_one() {
    ZRLOG_INFO("Hello World, the answer is {} and {:.2f}", 42, 3.14);
}

void drain() {
    g_logger->fini();
}

}  // namespace

int main(int argc, char** argv) {
    bench::Backend b;
    b.name = "zrlog";
    b.init = &init;
    b.log_one = &log_one;
    b.drain = &drain;
    return bench::run_suite(b, argc, argv);
}
