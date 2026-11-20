// fmtlog 后端适配
// fmtlog 默认队列满即丢弃（FMTLOG_BLOCK=0），与 zrlog Discard 语义对齐。
// 用 startPollingThread 让后台线程负责格式化与写盘，与其它库一致。
//
// 队列容量对等：fmtlog 默认 FMTLOG_QUEUE_SIZE = 1MB，而 bench_zrlog 用 8MB
// 线程缓冲。容量不同会让"满队列后丢弃多少"这个变量干扰吞吐与尾部对比，
// 故这里统一到 8MB（必须在 #include "fmtlog.h" 之前定义）。
// 设 FMTLOG_BENCH_QUEUE_MB=1 可退回 fmtlog 默认容量做对照实验。
#include "bench_common.h"

#if defined(FMTLOG_BENCH_QUEUE_MB)
#define FMTLOG_QUEUE_SIZE (FMTLOG_BENCH_QUEUE_MB << 20)
#else
#define FMTLOG_QUEUE_SIZE (8 << 20)
#endif

#include "fmtlog.h"

namespace {

void init(const char* logfile) {
    std::remove(logfile);
    fmtlog::setLogFile(logfile, true);
    fmtlog::setLogLevel(fmtlog::INF);
    // 起后台轮询线程：zrlog/spdlog/nanolog 都自带后台消费者，fmtlog 默认
    // 需要用户 poll。为架构对等，这里统一用后台线程（不占业务线程）。
    fmtlog::startPollingThread(1000000);  // 1ms 轮询
}

void log_one() {
    logi("Hello World, the answer is {} and {:.2f}", 42, 3.14);
}

void poll() {}

void drain() {
    fmtlog::stopPollingThread();
    fmtlog::poll(true);
}

}  // namespace

int main(int argc, char** argv) {
    bench::Backend b;
    b.name = "fmtlog";
    b.init = &init;
    b.log_one = &log_one;
    b.drain = &drain;
    b.poll = &poll;
    return bench::run_suite(b, argc, argv);
}
