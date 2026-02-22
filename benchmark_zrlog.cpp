// ZrLog Benchmark Test
// Compile: g++ -O3 -std=c++17 -pthread benchmark_zrlog.cpp -o benchmark_zrlog

#include "zrlog.h"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <fstream>

// Get TSC frequency for timing
static double get_tsc_freq_ghz() {
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t tsc0 = zrlog_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t tsc1 = zrlog_rdtsc();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double tsc_delta = static_cast<double>(tsc1 - tsc0);
    return (tsc_delta / ns);
}

// Benchmark: Measure single log call latency
void benchmark_frontend_latency(int iterations = 1000000) {
    std::cout << "\n=== Frontend Latency Test ===" << std::endl;

    std::vector<uint64_t> latencies;
    latencies.reserve(iterations);

    // Warm up
    for (int i = 0; i < 1000; ++i) {
        ZRLOG_INFO("Warmup message %d", i);
    }

    // Measure
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = zrlog_rdtsc();
        ZRLOG_INFO("Test message %d", i);
        uint64_t t1 = zrlog_rdtsc();
        if (i > 0) {
            latencies.push_back(t1 - t0);
        }
    }

    // Calculate statistics
    std::sort(latencies.begin(), latencies.end());

    double tsc_ghz = get_tsc_freq_ghz();

    auto to_ns = [tsc_ghz](uint64_t tsc) { return tsc / tsc_ghz; };

    double avg_tsc = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    uint64_t min_tsc = latencies.front();
    uint64_t max_tsc = latencies.back();
    uint64_t p50_tsc = latencies[latencies.size() * 50 / 100];
    uint64_t p90_tsc = latencies[latencies.size() * 90 / 100];
    uint64_t p99_tsc = latencies[latencies.size() * 99 / 100];
    uint64_t p999_tsc = latencies[latencies.size() * 999 / 1000];

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "TSC Frequency: " << tsc_ghz << " GHz" << std::endl;
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Average: " << to_ns(avg_tsc) << " ns" << std::endl;
    std::cout << "Min:     " << to_ns(min_tsc) << " ns" << std::endl;
    std::cout << "Max:     " << to_ns(max_tsc) << " ns" << std::endl;
    std::cout << "P50:     " << to_ns(p50_tsc) << " ns" << std::endl;
    std::cout << "P90:     " << to_ns(p90_tsc) << " ns" << std::endl;
    std::cout << "P99:     " << to_ns(p99_tsc) << " ns" << std::endl;
    std::cout << "P99.9:   " << to_ns(p999_tsc) << " ns" << std::endl;
}

// Benchmark: Measure throughput
void benchmark_throughput(int total_logs = 10000000) {
    std::cout << "\n=== Throughput Test (Single Thread) ===" << std::endl;

    // Warm up
    for (int i = 0; i < 1000; ++i) {
        ZRLOG_INFO("Warmup %d", i);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < total_logs; ++i) {        
        ZRLOG_INFO("Throughput test message %d", i);
    }
    auto end = std::chrono::high_resolution_clock::now();

    double seconds = std::chrono::duration<double>(end - start).count();
    double throughput = total_logs / seconds / 1000000.0; // Million/sec

    std::cout << "Total logs: " << total_logs << std::endl;
    std::cout << "Time: " << std::fixed << std::setprecision(3) << seconds << " seconds" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << throughput << " M logs/sec" << std::endl;
}

// Benchmark: Multi-thread throughput
void benchmark_multi_thread(int num_threads, int logs_per_thread) {
    std::cout << "\n=== Multi-Thread Throughput Test ===" << std::endl;
    std::cout << "Threads: " << num_threads << ", Logs per thread: " << logs_per_thread << std::endl;

    std::atomic<int> ready{ 0 };
    std::atomic<bool> start_flag{ false };

    auto worker = [&](int thread_id) {
        // Warm up
        for (int i = 0; i < 100; ++i) {
            ZRLOG_INFO("Warmup thread %d msg %d", thread_id, i);
        }

        ready.fetch_add(1);
        while (!start_flag.load()) {
            std::this_thread::yield();
        }

        for (int i = 0; i < logs_per_thread; ++i) {
            ZRLOG_INFO("Thread %d message %d", thread_id, i);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }

    // Wait for all threads ready
    while (ready.load() < num_threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto start = std::chrono::high_resolution_clock::now();
    start_flag.store(true);

    for (auto& t : threads) {
        t.join();
    }
    auto end = std::chrono::high_resolution_clock::now();

    int total_logs = num_threads * logs_per_thread;
    double seconds = std::chrono::duration<double>(end - start).count();
    double throughput = total_logs / seconds / 1000000.0;

    std::cout << "Total logs: " << total_logs << std::endl;
    std::cout << "Time: " << std::fixed << std::setprecision(3) << seconds << " seconds" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << throughput << " M logs/sec" << std::endl;
}

// Benchmark: Different message types
void benchmark_message_types(int iterations = 100000) {
    std::cout << "\n=== Message Type Latency Test ===" << std::endl;

    std::vector<uint64_t> int_latencies, str_latencies, mixed_latencies;
    int_latencies.reserve(iterations);
    str_latencies.reserve(iterations);
    mixed_latencies.reserve(iterations);

    // Warm up
    for (int i = 0; i < 1000; ++i) {
        ZRLOG_INFO("%d", i);
        ZRLOG_INFO("%s", "string");
        ZRLOG_INFO("%d %s %d", i, "test", i * 2);
    }

    double tsc_ghz = get_tsc_freq_ghz();
    auto to_ns = [tsc_ghz](uint64_t tsc) { return tsc / tsc_ghz; };

    // Integer only
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = zrlog_rdtsc();
        ZRLOG_INFO("Integer value: %d", i);
        uint64_t t1 = zrlog_rdtsc();
        if (i > 0) {
            int_latencies.push_back(t1 - t0);
        }
    }

    // String
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = zrlog_rdtsc();
        ZRLOG_INFO("String value: %s", "benchmark_test_string");
        uint64_t t1 = zrlog_rdtsc();
        if (i > 0) {
            str_latencies.push_back(t1 - t0);
        }
    }

    // Mixed
    for (int i = 0; i < iterations; ++i) {
        uint64_t t0 = zrlog_rdtsc();
        ZRLOG_INFO("Mixed: %d %f %s %d", i, 3.14, "text", i * 100);
        uint64_t t1 = zrlog_rdtsc();
        if (i > 0) {
            mixed_latencies.push_back(t1 - t0);
        }
    }

    auto calc_stats = [&](const std::vector<uint64_t>& latencies) {
        std::vector<uint64_t> sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        double avg = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();
        return std::make_tuple(to_ns(avg), to_ns(sorted[sorted.size() * 50 / 100]), to_ns(sorted[sorted.size() * 99 / 100]));
    };

    auto [int_avg, int_p50, int_p99] = calc_stats(int_latencies);
    auto [str_avg, str_p50, str_p99] = calc_stats(str_latencies);
    auto [mix_avg, mix_p50, mix_p99] = calc_stats(mixed_latencies);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Message Type    | Avg (ns) | P50 (ns) | P99 (ns)" << std::endl;
    std::cout << "Integer only    | " << std::setw(8) << int_avg << " | " << std::setw(8) << int_p50 << " | " << std::setw(8) << int_p99 << std::endl;
    std::cout << "String          | " << std::setw(8) << str_avg << " | " << std::setw(8) << str_p50 << " | " << std::setw(8) << str_p99 << std::endl;
    std::cout << "Mixed types     | " << std::setw(8) << mix_avg << " | " << std::setw(8) << mix_p50 << " | " << std::setw(8) << mix_p99 << std::endl;
}

// Benchmark: TSC Clock vs system clock
void benchmark_clock() {
    std::cout << "\n=== Clock Performance Test ===" << std::endl;

    const int iterations = 1000000;

    // TSC clock
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        volatile uint64_t t = zrlog::TscClock::now_ns_i();
        (void)t;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // System clock
    auto t2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        volatile auto t = std::chrono::system_clock::now().time_since_epoch().count();
        (void)t;
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    double tsc_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iterations;
    double sys_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / iterations;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "TSC Clock:    " << tsc_ns << " ns/call" << std::endl;
    std::cout << "System Clock: " << sys_ns << " ns/call" << std::endl;
    std::cout << "Speedup:      " << (sys_ns / tsc_ns) << "x" << std::endl;
}

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "        zrlog Performance Benchmark        "  << std::endl;
    std::cout << "============================================" << std::endl;

    // Remove old log file
    std::remove("benchmark_zrlog.log");

    // Initialize logger
    zrlog::Config config;
    config.filename = "benchmark_zrlog.log";
    config.level = zrlog::LogLevel::INFO;
    config.thread_buffer_size = 16 * 1024 * 1024;  // 16MB per thread
    config.io_buffer_size = 1024 * 512;  // 512KB IO buffer

    if (!zrlog::NanoLogger::instance().init(config)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }

    // Run benchmarks
    benchmark_clock();
    benchmark_frontend_latency(500000);
    benchmark_message_types(100000);
    benchmark_throughput(5000000);
    benchmark_multi_thread(2, 1000000);
    benchmark_multi_thread(4, 1000000);
    benchmark_multi_thread(8, 500000);

    // Shutdown
    ZRLOG_FINI();

    std::cout << "\n=== Benchmark Complete ===" << std::endl;
    std::cout << "Log file: benchmark_zrlog.log" << std::endl;

    return 0;
}
