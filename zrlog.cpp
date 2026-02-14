#include "zrlog.h"
#include <vector>

void bench_thread(int id, int times) {
    std::string str = "abcdefghijklmnopqrstwyz";
    ZRLOG_INFO("Thread:%d times:%d value:%.2f string:%s", id, times, 3.14159, str);
    //ZRLOG_INFO("Thread:%d begin...", id);
    //ZRLOG_INFO("Thread:%d iteration:%d value:%.2f string:%s", id, i, 3.14159, "test");
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < times; ++i) {
        //ZRLOG_INFO("Thread:%d iteration:%d value:%.2f string:%s", id, i, 3.14159, "test");
        ZRLOG_INFO("Thread:%d iteration:%d", id, i);
    }
    auto end = std::chrono::steady_clock::now();
    auto diff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    printf("thread:%d times:%d in %lld ns, per_times_ns:%.6f\n", id, times, diff_ns, diff_ns*1.0/times);
}

int main(int argc, char* argv[]) {

    int thread_num = 4;
    int lines_per_thread = 100000;
    if (argc > 1) {
        thread_num = atoi(argv[1]);
        if (thread_num < 0) {
            thread_num = 1;
        }
    }
    if (argc > 2) {
        lines_per_thread = atoi(argv[2]);
        if (lines_per_thread < 1) {
            lines_per_thread = 1;
        }
    }

    zrlog::Config conf;
    conf.appender = zrlog::AppenderType::File;
    conf.filename = "test_benchmark.log";
    conf.level = zrlog::LogLevel::DEBUG;
    ZRLOG_INIT_CONF(conf);
    //ZRLOG_INIT("test_benchmark.log", zrlog::LogLevel::DEBUG);

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < thread_num; ++i) {
        threads.emplace_back(bench_thread, i, lines_per_thread);
    }
    for (auto& t : threads) {
        t.join();
    }

    // 等待后台写完（实际使用中不需要手动sleep，fini会自动join）
    ZRLOG_FINI();

    auto end = std::chrono::steady_clock::now();
    auto diff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    auto log_lines = static_cast<uint64_t>(lines_per_thread) * thread_num;
    printf("flush_log thread_num:%d, lines_per_thread:%d Logged %lld lines in %lld ns, per_line_ns:%.6f\n", 
        thread_num, lines_per_thread, log_lines, diff_ns, diff_ns*1.0/log_lines);

    return 0;
}
