#include "zrlog.h"
#include <vector>

void bench_thread(int id, int times_per_thread) {
    for (int i = 0; i < times_per_thread; ++i) {
        ZRLOG_INFO("Thread %d iteration %d value %.2f string %s", id, i, 3.14159, "test");
    }
}

int main() {
    zrlog::Config conf;
    conf.appender = zrlog::AppenderType::File;
    conf.filename = "test_benchmark.log";
    conf.level = zrlog::LogLevel::DEBUG;
    ZRLOG_INIT_CONF(conf);
    //ZRLOG_INIT("test_benchmark.log", zrlog::LogLevel::DEBUG);

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();

    int thread_num = 100;
    int times_per_thread = 10000;
    for (int i = 0; i < thread_num; ++i) {
        threads.emplace_back(bench_thread, i, times_per_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    // 等待后台写完（实际使用中不需要手动sleep，fini会自动join）
    ZRLOG_FINI();

    auto end = std::chrono::steady_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto log_lines = static_cast<uint64_t>(times_per_thread * thread_num);
    printf("Logged %lld lines in %lld us\n", log_lines, dur);

    return 0;
}
