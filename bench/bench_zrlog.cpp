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
    // 缓冲区调大：线程缓冲 8MB -> 32MB，IO 缓冲 8MB -> 32MB。
    // 更大的线程缓冲让前端更晚撞满（少走 alloc 慢路径），更大的 IO 缓冲让
    // 后端更少 flush（少阻塞在 write/fsync）。
    // cfg.thread_buffer_size = 8 * 1024 * 1024;
    // cfg.io_buffer_size = 8 * 1024 * 1024;
    cfg.thread_buffer_size = 8 * 1024 * 1024;
    cfg.io_buffer_size = 8 * 1024 * 1024;
    // 诊断用：ZRLOG_BENCH_TB_MB 覆盖线程缓冲大小（MB），用于验证"缓冲接近满
    // → alloc 走跨核慢路径"这一假设。
    if (const char* tb = std::getenv("ZRLOG_BENCH_TB_MB")) {
        if (*tb) cfg.thread_buffer_size = static_cast<uint32_t>(std::atoi(tb)) * 1024 * 1024;
    }
    cfg.buffer_full_policy = zrlog::BufferFullPolicy::Discard;
    // 关掉前端两条可选开销，测纯序列化+入队的下限：
    //  - 不再每条日志检查空闲标志（消费者靠 idle 超时轮询）
    //  - 不再累加生产统计（stat_produce_* 恒为 0）
    cfg.notify_consumer_per_log = false;
    cfg.producer_stats = false;
    // 诊断用：ZRLOG_BENCH_BG_CORE 指定后端线程绑核（如 10=其他 P 核、
    // 1=生产者 CPU 0 的超线程兄弟、2=E 核）。空 = 不绑（默认行为）。
    if (const char* bg = std::getenv("ZRLOG_BENCH_BG_CORE")) {
        if (*bg) cfg.background_thread_core_ids.push_back(std::atoi(bg));
    }
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
