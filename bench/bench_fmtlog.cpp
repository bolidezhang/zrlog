// fmtlog 后端适配
// fmtlog 默认队列满即丢弃（FMTLOG_BLOCK=0），与 zrlog Discard 语义对齐。
// 用 startPollingThread 让后台线程负责格式化与写盘，与其它库一致。
#include "bench_common.h"

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
