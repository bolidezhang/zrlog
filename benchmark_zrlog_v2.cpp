#include "zrlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Percentiles {
    double avg_ns{0.0};
    double p50_ns{0.0};
    double p90_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double min_ns{0.0};
    double max_ns{0.0};
};

struct ThroughputResult {
    int threads{0};
    std::uint64_t produced{0};
    std::uint64_t valid{0};
    std::uint64_t dropped{0};
    double seconds{0.0};
    double mlogs_per_sec{0.0};
    double drop_ratio_pct{0.0};
};

double calibrate_tsc_cycles_per_ns() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = zrlog_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t c1 = zrlog_rdtsc();

    const double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / elapsed_ns;
}

Percentiles calc_percentiles(std::vector<std::uint64_t> cycles, double cycles_per_ns) {
    std::sort(cycles.begin(), cycles.end());

    auto pick = [&](double ratio) -> double {
        const std::size_t index = static_cast<std::size_t>(ratio * (cycles.size() - 1));
        return static_cast<double>(cycles[index]) / cycles_per_ns;
    };

    const double avg_cycles = std::accumulate(cycles.begin(), cycles.end(), 0.0) / static_cast<double>(cycles.size());
    return Percentiles{
        avg_cycles / cycles_per_ns,
        pick(0.50),
        pick(0.90),
        pick(0.99),
        pick(0.999),
        static_cast<double>(cycles.front()) / cycles_per_ns,
        static_cast<double>(cycles.back()) / cycles_per_ns,
    };
}

Percentiles run_frontend_latency(std::size_t iterations, std::size_t warmup, double cycles_per_ns) {
    for (std::size_t i = 0; i < warmup; ++i) {
        ZRLOG_INFO("warmup-latency i=%zu", i);
    }

    std::vector<std::uint64_t> cycles;
    cycles.reserve(iterations);

    for (std::size_t i = 0; i < iterations; ++i) {
        const std::uint64_t t0 = zrlog_rdtsc();
        ZRLOG_INFO("latency i=%zu v=%d", i, static_cast<int>(i & 1023));
        const std::uint64_t t1 = zrlog_rdtsc();
        cycles.push_back(t1 - t0);
    }

    return calc_percentiles(std::move(cycles), cycles_per_ns);
}

std::vector<int> parse_threads(const std::string &text) {
    std::vector<int> threads;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const int value = std::atoi(item.c_str());
        if (value > 0) {
            threads.push_back(value);
        }
    }
    if (threads.empty()) {
        threads = {1, 2, 4, 8};
    }
    return threads;
}

ThroughputResult run_throughput_for_threads(int thread_count, int seconds, std::size_t warmup_per_thread,
                                            zrlog::NanoLogger &logger) {
    for (std::size_t i = 0; i < warmup_per_thread; ++i) {
        ZRLOG_INFO("warmup-throughput i=%zu", i);
    }

    const auto produce_before = logger.stat_produce_count_.load(std::memory_order_relaxed);
    const auto valid_before = logger.stat_produce_valid_count_.load(std::memory_order_relaxed);
    const auto drop_before = logger.stat_produce_drop_count_.load(std::memory_order_relaxed);

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    std::vector<std::uint64_t> local_counts(static_cast<std::size_t>(thread_count), 0);
    workers.reserve(static_cast<std::size_t>(thread_count));

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::uint64_t local = 0;
            while (!stop.load(std::memory_order_acquire)) {
                ZRLOG_INFO("thr=%d seq=%llu", t, static_cast<unsigned long long>(local));
                ++local;
            }
            local_counts[static_cast<std::size_t>(t)] = local;
        });
    }

    while (ready.load(std::memory_order_acquire) < thread_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true, std::memory_order_release);

    for (auto &worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();

    const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();
    const std::uint64_t total_local = std::accumulate(local_counts.begin(), local_counts.end(), std::uint64_t{0});

    const auto produce_after = logger.stat_produce_count_.load(std::memory_order_relaxed);
    const auto valid_after = logger.stat_produce_valid_count_.load(std::memory_order_relaxed);
    const auto drop_after = logger.stat_produce_drop_count_.load(std::memory_order_relaxed);

    const std::uint64_t produced = produce_after - produce_before;
    const std::uint64_t valid = valid_after - valid_before;
    const std::uint64_t dropped = drop_after - drop_before;
    const double drop_ratio = produced > 0 ? (100.0 * static_cast<double>(dropped) / static_cast<double>(produced)) : 0.0;

    return ThroughputResult{thread_count,
                            std::max(total_local, produced),
                            valid,
                            dropped,
                            elapsed_seconds,
                            static_cast<double>(std::max(total_local, produced)) / elapsed_seconds / 1'000'000.0,
                            drop_ratio};
}

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [thread_buffer_mb] [policy(0/1/2)] [throughput_seconds] [threads_csv]\n";
    std::cout << "Example: " << prog << " 4 0 2 1,2,4,8\n";
}

} // namespace

int main(int argc, char *argv[]) {
    double thread_buffer_mb = 4.0;
    zrlog::BufferFullPolicy policy = zrlog::BufferFullPolicy::Discard;
    int throughput_seconds = 2;
    std::string threads_csv = "1,2,4,8";

    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_usage(argv[0]);
        return 0;
    }

    if (argc > 1) {
        thread_buffer_mb = std::atof(argv[1]);
        if (thread_buffer_mb <= 0.0) {
            thread_buffer_mb = 4.0;
        }
    }
    if (argc > 2) {
        const int policy_value = std::atoi(argv[2]);
        if (policy_value >= 0 && policy_value <= static_cast<int>(zrlog::BufferFullPolicy::Retry)) {
            policy = static_cast<zrlog::BufferFullPolicy>(policy_value);
        }
    }
    if (argc > 3) {
        throughput_seconds = std::max(1, std::atoi(argv[3]));
    }
    if (argc > 4) {
        threads_csv = argv[4];
    }

    std::remove("benchmark_zrlog_v2.log");

    zrlog::Config config;
    config.filename = "benchmark_zrlog_v2.log";
    config.level = zrlog::LogLevel::INFO;
    config.thread_buffer_size = static_cast<std::uint32_t>(1024 * 1024 * thread_buffer_mb);
    config.io_buffer_size = 1024 * 1024;
    config.buffer_full_policy = policy;

    auto &logger = zrlog::NanoLogger::instance();
    if (!logger.init(config)) {
        std::cerr << "failed to init logger\n";
        return 1;
    }

    const double cycles_per_ns = calibrate_tsc_cycles_per_ns();
    const Percentiles latency = run_frontend_latency(250000, 5000, cycles_per_ns);
    const std::vector<int> thread_counts = parse_threads(threads_csv);

    std::vector<ThroughputResult> throughput_results;
    throughput_results.reserve(thread_counts.size());
    for (int tc : thread_counts) {
        throughput_results.push_back(run_throughput_for_threads(tc, throughput_seconds, 2000, logger));
    }

    ZRLOG_FINI();

    std::cout << "\n==== zrlog benchmark v2 ====\n";
    std::cout << "buffer_mb=" << thread_buffer_mb << " policy=" << static_cast<int>(policy)
              << " throughput_seconds=" << throughput_seconds << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "frontend latency(ns): avg=" << latency.avg_ns << " min=" << latency.min_ns << " p50=" << latency.p50_ns
              << " p90=" << latency.p90_ns << " p99=" << latency.p99_ns << " p99.9=" << latency.p999_ns
              << " max=" << latency.max_ns << "\n";

    std::cout << "\nthroughput summary\n";
    std::cout << "threads | Mlogs/s | produced | valid | dropped | drop% | seconds\n";
    for (const auto &r : throughput_results) {
        std::cout << std::setw(7) << r.threads << " | " << std::setw(7) << r.mlogs_per_sec << " | " << std::setw(8)
                  << r.produced << " | " << std::setw(8) << r.valid << " | " << std::setw(8) << r.dropped << " | "
                  << std::setw(6) << r.drop_ratio_pct << " | " << r.seconds << "\n";
    }

    std::cout << "\nfinal counters: produce=" << logger.stat_produce_count_.load(std::memory_order_relaxed)
              << " valid=" << logger.stat_produce_valid_count_.load(std::memory_order_relaxed)
              << " consume=" << logger.stat_consume_count_.load(std::memory_order_relaxed)
              << " consume_valid=" << logger.stat_consume_valid_count_.load(std::memory_order_relaxed)
              << " dropped=" << logger.stat_produce_drop_count_.load(std::memory_order_relaxed) << "\n";

    return 0;
}
