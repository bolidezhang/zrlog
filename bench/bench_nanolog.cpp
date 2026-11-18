// NanoLog 后端适配（C++17 版本，printf 风格 API）
// 注意：NanoLog 输出二进制日志，需 decompressor 才能还原为文本；
// 本基准只比较前端调用开销与吞吐，不比较落盘格式。
#include "bench_common.h"

#include "NanoLogCpp17.h"

using namespace NanoLog::LogLevels;

namespace {

void init(const char* logfile) {
    std::remove(logfile);
    NanoLog::setLogFile(logfile);
    NanoLog::setLogLevel(NOTICE);
}

void log_one() {
    NANO_LOG(NOTICE, "Hello World, the answer is %d and %.2f", 42, 3.14);
}

void drain() {
    NanoLog::sync();
}

void pre_thread() {
    NanoLog::preallocate();
}

}  // namespace

int main(int argc, char** argv) {
    bench::Backend b;
    b.name = "nanolog";
    b.init = &init;
    b.log_one = &log_one;
    b.drain = &drain;
    b.pre_thread = &pre_thread;
    return bench::run_suite(b, argc, argv);
}
