// quill 后端适配
//
// quill 的架构与 zrlog 接近：每线程前端队列 + 后台 backend 线程格式化落盘。
// 需要注意：
//  - 必须先 Backend::start() 启动后台线程，否则日志只进队列不落盘。
//  - quill 默认按固定间隔轮询队列（sleep_duration），这里设为最小值以减少
//    落盘延迟，与其它库的后台消费线程行为对齐。
//  - 退出前要 flush_log() 并 stop()，否则尾部日志可能丢失。
#include "bench_common.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"

#include <chrono>
#include <memory>
#include <string>

namespace {

// 自定义前端选项：
//  - UnboundedBlocking：队列满时扩容而非阻塞。曾试过 BoundedBlocking + 2MB，
//    队列一满前端就阻塞等后端，1600 万条要跑 28 秒（0.5 M/s），完全失真。
//  - 初始容量 32MB：quill 扩容时要分配新队列并拷贝旧数据，实测那次调用会
//    阻塞 ~3.7ms。延迟测试共 20 万条（约 8.4MB），默认 128KB/8MB 都会在测试
//    中途扩容，把 max 和 avg 一起污染（avg 会被单个 3.7ms 离群点抬高 ~19ns）。
//    32MB 保证延迟测试全程不扩容，测的是稳态前端开销。
struct BenchFrontendOptions : quill::FrontendOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
    static constexpr size_t initial_queue_capacity = 32u * 1024u * 1024u;
    static constexpr bool enable_immediate_flush = false;
};

using BenchLogger = quill::LoggerImpl<BenchFrontendOptions>;

BenchLogger* g_logger = nullptr;

void init(const char* logfile) {
    std::remove(logfile);

    quill::BackendOptions backend_options;
    // 用 quill 自己的默认轮询间隔（100us）。曾试过 100ns，后端线程会空转抢
    // CPU，导致前端延迟 max 飙到毫秒级——反而更差。
    backend_options.sleep_duration = std::chrono::microseconds{100};
    // 关闭时间戳排序宽限期：默认 5us 会让后端等够宽限期才处理，拖慢落盘。
    // 本基准所有日志来自同一进程，不需要跨线程严格排序。
    backend_options.log_timestamp_ordering_grace_period = std::chrono::microseconds{0};
    quill::Backend::start(backend_options);

    auto sink = quill::FrontendImpl<BenchFrontendOptions>::create_or_get_sink<quill::FileSink>(
        logfile,
        [] {
            quill::FileSinkConfig cfg;
            cfg.set_open_mode('w');
            cfg.set_filename_append_option(quill::FilenameAppendOption::None);
            return cfg;
        }());

    g_logger = quill::FrontendImpl<BenchFrontendOptions>::create_or_get_logger(
        "bench", std::move(sink));
    g_logger->set_log_level(quill::LogLevel::Info);
}

void log_one() {
    LOG_INFO(g_logger, "Hello World, the answer is {} and {:.2f}", 42, 3.14);
}

void drain() {
    if (g_logger) {
        g_logger->flush_log();
    }
    quill::Backend::stop();
}

}  // namespace

int main(int argc, char** argv) {
    bench::Backend b;
    b.name = "quill";
    b.init = &init;
    b.log_one = &log_one;
    b.drain = &drain;
    return bench::run_suite(b, argc, argv);
}
