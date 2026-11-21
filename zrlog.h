/*
Copyright (c) 2024 Bolide Zhang <bolidezhang@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

// The zrlog library version in the form major * 10000 + minor * 100 + patch.
// 如：2.5.3
#define ZRLOG_VERSION 20503

// ============================================================================
// 日志头部统一格式定义 (用于 FMT_COMPILE 极速渲染)
// 参数依次为: 1.时间字符串 2.纳秒 3.日志级别 4.线程ID 5.文件名 6.行号
// ============================================================================
#define ZRLOG_HEADER_FMT "{}.{:09d} {} {} {}:{} "

// ============================================================================
// 引入 fmtlib (Header-Only 模式 & 编译期优化支持)
// ============================================================================
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/compile.h>  // 必须引入，以支持 FMT_COMPILE 宏

#include <cstdlib>
#include <cstdio>
#include <cinttypes>
#include <cerrno>
#include <ctime>
#include <csignal>        // 用于崩溃信号捕获
#include <filesystem>
#include <vector>
#include <deque>
#include <list>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <string_view>
#include <cstring>
#include <tuple>
#include <chrono>
#include <functional>
#include <algorithm>
#include <type_traits>
#include <charconv>
#include <memory>
#include <new>
#include <unordered_map>

// 获取当前 CPU 架构的 L1 Cache Line 大小 (C++17)
#ifdef __cpp_lib_hardware_interference_size
constexpr size_t ZRLOG_CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
constexpr size_t ZRLOG_CACHE_LINE_SIZE = 64; // 兼容老编译器的 Fallback
#endif

// 跨核心高频写字段之间的隔离步长。Intel 的 L2 adjacent-cache-line prefetcher
// 按 128 字节成对拉取缓存行，因此两个不同核心各自写入的字段只隔一条 64 字节
// 行时，仍会互相触发 RFO。保守取 128；非 Intel 平台略增内存占用，无害。
constexpr size_t ZRLOG_CROSS_THREAD_STRIDE = 128;

// 跨平台分支预测与函数内联控制。热/冷拆分用 ZRLOG_NOINLINE 保留清晰边界，
// 不让首次创建缓冲、分配失败等冷代码撑大热函数的寄存器分配与栈帧。
#if defined(__GNUC__) || defined(__clang__)
#define ZRLOG_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ZRLOG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ZRLOG_FORCEINLINE inline __attribute__((always_inline))
#define ZRLOG_NOINLINE    __attribute__((noinline))
#else
#define ZRLOG_LIKELY(x)   (x)
#define ZRLOG_UNLIKELY(x) (x)
#define ZRLOG_FORCEINLINE __forceinline
#define ZRLOG_NOINLINE    __declspec(noinline)
#endif

// ---------------------------------------------------------------------------
// 平台差异处理 & 内部宏定义
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <share.h>
#include <intrin.h>

#define ZRLOG_OPEN(path, flags, mode)   _open(path, flags, mode)
#define ZRLOG_WRITE(fd, data, len)      _write(fd, data, (unsigned int)(len))
#define ZRLOG_CLOSE(fd)                 _close(fd)
#define ZRLOG_FLUSH_FILE(fd)            _commit(fd)
#define ZRLOG_ACCESS(path, mode)        _access(path, mode)
#define ZRLOG_O_FLAGS                   (_O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY)
#define ZRLOG_S_FLAGS                   (_S_IREAD | _S_IWRITE)
#define ZRLOG_F_OK                      0
#define ZRLOG_FAST_THREAD_LOCAL         __declspec(thread)

//标准输入 keyboard(defaut)
#define STDIN_FILENO  0

//标准输出 screen(defaut)
#define STDOUT_FILENO 1

//标准错误输出 screen(defaut)
#define STDERR_FILENO 2

inline void zrlog_gmtime(const time_t* timer, struct tm* buf) {
    gmtime_s(buf, timer);
}

inline void zrlog_localtime(const time_t* timer, struct tm* buf) {
    localtime_s(buf, timer);
}

// --- MSVC 上的架构分支：x86/x64 用 rdtsc/pause，ARM 用 CNTVCT_EL0 ---
// ARM64_CNTVCT_EL0 定义在 Windows SDK 的 winnt.h（<Windows.h> 已包含）。
// YieldProcessor() 是 winnt.h 提供的跨架构自旋提示（x86=_mm_pause，
// ARM/ARM64=__yield），比手写 __isb 更贴合"让出流水线"的语义。
#if defined(_M_ARM64) || defined(_M_ARM64EC)
// ARM64 没有 rdtsc 等价指令；CNTVCT_EL0 是系统计数器（频率固定，
// 通常 19.24~62.5MHz），语义与 TSC 一致（单调递增、跨核同步），
// 由 calibrate() 的 ns/tick 换算吸收频率差异。
#define ZRLOG_CPU_PAUSE()               YieldProcessor()
inline uint64_t zrlog_rdtsc() {
    return _ReadStatusReg(ARM64_CNTVCT_EL0);
}
#elif defined(_M_ARM)
// 32 位 ARM：无 CNTVCT 常量，退化为系统高精度时钟（较慢但可用）。
#define ZRLOG_CPU_PAUSE()               YieldProcessor()
inline uint64_t zrlog_rdtsc() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}
#else
#define ZRLOG_CPU_PAUSE()               _mm_pause()
inline uint64_t zrlog_rdtsc() {
    return __rdtsc();
}
#endif

#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <pthread.h>
#include <sched.h>

#ifdef __x86_64__
#include <x86intrin.h>
#endif

#if defined(__linux__)
#include <sys/mman.h>
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#endif

#define ZRLOG_OPEN(path, flags, mode)   ::open(path, flags, mode)
#define ZRLOG_WRITE(fd, data, len)      ::write(fd, data, len)
#define ZRLOG_CLOSE(fd)                 ::close(fd)
#define ZRLOG_FLUSH_FILE(fd)            ::fsync(fd)
#define ZRLOG_ACCESS(path, mode)        ::access(path, mode)
#define ZRLOG_O_FLAGS                   (O_WRONLY | O_CREAT | O_APPEND)
#define ZRLOG_S_FLAGS                   (0644)
#define ZRLOG_F_OK                      F_OK
#define ZRLOG_FAST_THREAD_LOCAL         __thread

#if defined(__x86_64__) || defined(__i386__)
#define ZRLOG_CPU_PAUSE() __asm__ volatile("pause")
inline uint64_t zrlog_rdtsc() {
    return __builtin_ia32_rdtsc();
}
#elif defined(__aarch64__)
#define ZRLOG_CPU_PAUSE() __asm__ volatile("yield")
inline uint64_t zrlog_rdtsc() {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}
#else
#define ZRLOG_CPU_PAUSE() std::this_thread::yield()
inline uint64_t zrlog_rdtsc() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}
#endif

inline void zrlog_gmtime(const time_t* timer, struct tm* buf) {
    gmtime_r(timer, buf);
}

inline void zrlog_localtime(const time_t* timer, struct tm* buf) {
    localtime_r(timer, buf);
}
#endif

namespace zrlog {

    //使用方法:
    //ZRLOG_INFO("System error: {}", zrlog::literal("Database connection lost"));

    // 静态字符串(字面量)
    struct string_literal_t {
        const char* ptr;
        uint32_t len;
    };

    // 包装函数。
    // ⚠️ 安全约束：str 必须指向【静态存储期】内存（字符串字面量、全局/静态数组）。
    // 本函数只拷贝指针+长度，后端异步解码时才解引用——若包装栈上局部数组
    // （如 char buf[64]; literal(buf)），日志调用返回后栈帧销毁，后端读到悬垂
    // 指针（UAF）。C++17 无法在编译期强制存储期，故靠此约定；字符串字面量请优先
    // 用更安全的 "..."_sl 后缀（只能作用于字面量，天然杜绝栈数组误用）。
    template <size_t N>
    inline constexpr string_literal_t literal(const char(&str)[N]) noexcept {
        return { str, static_cast<uint32_t>(N > 0 ? N - 1 : 0) };
    }

    inline namespace literals {
        //使用方法:
        //ZRLOG_INFO("System error: {}", "Database connection lost"_sl);

        // 提供 C++11后缀糖字面量(Syntactic Sugar)，让代码更优雅
        inline constexpr string_literal_t operator""_sl(const char* str, size_t len) noexcept {
            return { str, static_cast<uint32_t>(len) };
        }
    }

    // =========================================================================
    // format_as（利用 ADL 机制，必须写在 zrlog 命名空间内！）
    // 当 fmt 遇到 string_literal_t 时，转换为 std::string_view 保存，彻底消灭悬垂引用！
    // =========================================================================
    inline std::string_view format_as(const string_literal_t& sl) {
        return std::string_view(sl.ptr, sl.len);
    }

    // ---------------------------------------------------------------------------
    // 内部细节
    // ---------------------------------------------------------------------------
    namespace detail {

        // 编译期特征：判断 T 的真实类型（去除引用和 const/volatile 修饰后）是否为裸指针
        template <typename T>
        constexpr bool is_raw_char_ptr_v =
            std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, char*> ||
            std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, const char*>;

        // 编译期特征：参数能否被安全地按位序列化到环形缓冲。
        //
        // 前端是把参数逐位 memcpy 进缓冲、后端再 memcpy 出来，所以参数必须
        // "值语义可拷贝"：拷贝字节后与原件等价。std::string / std::vector 这类
        // 拥有堆内存的类型逐位拷贝后，后端拿到的是悬垂指针（UAF）。
        //
        // 默认规则：
        //   - 字符串类型（std::string / string_view / string_literal_t）由
        //     serialize_arg 专门处理（写长度 + 内容 / 指针 + 长度），放行；
        //   - 其余类型必须是 trivially copyable（标量、枚举、POD 结构体）。
        //
        // 逃生舱：确实需要传自定义类型时，可为该类型特化 loggable_arg 为
        // true_type，但必须自行保证"按值拷贝安全"（例如内部用定长数组而非堆
        // 指针，且不含指向外部生命周期的裸指针）。
        template <typename T, typename = void>
        struct loggable_arg
            : std::integral_constant<bool, std::is_trivially_copyable_v<std::decay_t<T>>> {};

        template <typename T>
        struct loggable_arg<T, std::enable_if_t<
            std::is_same_v<std::decay_t<T>, std::string> ||
            std::is_same_v<std::decay_t<T>, std::string_view> ||
            std::is_same_v<std::decay_t<T>, zrlog::string_literal_t>>> : std::true_type {};

        // 编译期类型映射 Traits：遇到字符串一律退化为 std::string_view
        template <typename T>
        struct decode_type {
            using type = std::conditional_t<
                std::is_same_v<T, const char*> || std::is_same_v<T, char*> ||
                std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>,
                std::string_view,
                T
            >;
        };

        // 指针漂移提取器 (反序列化)
        // end 为这条日志 payload 的末尾（不含），用于校验长度字段，防止损坏/错配
        // 数据导致越界读。越界时截断到可用长度，绝不读穿缓冲。
        template <typename T>
        static auto decode_arg(char*& ptr, const char* end) -> typename decode_type<std::decay_t<T>>::type {
            using DecayedT = std::decay_t<T>;

            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                zrlog::string_literal_t sl{ nullptr, 0 };
                if (static_cast<size_t>(end - ptr) >= sizeof(zrlog::string_literal_t)) {
                    std::memcpy(&sl, ptr, sizeof(zrlog::string_literal_t));
                    ptr += sizeof(zrlog::string_literal_t);
                }
                else {
                    ptr = const_cast<char*>(end);
                }
                return sl;
            }
            else if constexpr (std::is_same_v<DecayedT, const char*> || std::is_same_v<DecayedT, char*> ||
                std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                uint32_t len = 0;
                const size_t avail = static_cast<size_t>(end - ptr);
                if (avail < sizeof(uint32_t)) {
                    ptr = const_cast<char*>(end);
                    return std::string_view();
                }
                std::memcpy(&len, ptr, sizeof(uint32_t));
                ptr += sizeof(uint32_t);
                // 长度字段被破坏或类型错配时，len 可能远超剩余字节：截断到可用量
                const size_t remaining = static_cast<size_t>(end - ptr);
                if (len > remaining) {
                    len = static_cast<uint32_t>(remaining);
                }
                const char* str = ptr;
                ptr += len;
                return std::string_view(str, len);
            }
            else {
                DecayedT val{};
                if (static_cast<size_t>(end - ptr) >= sizeof(DecayedT)) {
                    std::memcpy(&val, ptr, sizeof(DecayedT));
                    ptr += sizeof(DecayedT);
                }
                else {
                    ptr = const_cast<char*>(end);
                }
                return val;
            }
        }

        // 快速转换 2 位数字 (00-99)
        inline void fast_u32_to_2digits(char* buf, uint32_t val) {
            static const char digits[201] =
                "00010203040506070809"
                "10111213141516171819"
                "20212223242526272829"
                "30313233343536373839"
                "40414243444546474849"
                "50515253545556575859"
                "60616263646566676869"
                "70717273747576777879"
                "80818283848586878889"
                "90919293949596979899";
            uint32_t off = val * 2;
            buf[0] = digits[off];
            buf[1] = digits[off + 1];
        }

        // 快速转换 4 位数字
        inline void fast_u32_to_4digits(char* buf, uint32_t val) {
            if (val >= 10000) {
                val = 9999;
            }
            fast_u32_to_2digits(buf, val / 100);
            fast_u32_to_2digits(buf + 2, val % 100);
        }

        // 快速转换 9 位纳秒，消除除以10的耗时循环，直接查表批量处理
        inline void fast_u32_to_9digits(char* buf, uint32_t val) {
            if (val > 999999999) val = 999999999;
            uint32_t d1 = val / 100000000;
            val %= 100000000;
            uint32_t d2 = val / 1000000;
            val %= 1000000;
            uint32_t d3 = val / 10000;
            val %= 10000;
            uint32_t d4 = val / 100;
            val %= 100;
            uint32_t d5 = val;
            buf[0] = (char)('0' + d1);
            fast_u32_to_2digits(buf + 1, d2);
            fast_u32_to_2digits(buf + 3, d3);
            fast_u32_to_2digits(buf + 5, d4);
            fast_u32_to_2digits(buf + 7, d5);
        }

        // FNV-1a 64 位哈希：用于 DYN 格式串变化检测的快路径（无锁、race-free）。
        // 修复：同一调用点的 DYN 格式串运行时可变（含 std::string 原地修改，data() 指针不变），
        // 必须按内容判断是否重注册，不能只比较指针。
        inline uint64_t fnv1a_hash(std::string_view s) noexcept {
            uint64_t h = 1469598103934665603ull;
            for (char c : s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return h;
        }
    }  //end namespace detail

    // ---------------------------------------------------------------------------
    // 基础工具类
    // ---------------------------------------------------------------------------
    class SpinMutex {
    public:
        void lock() {
            while (flag_.test_and_set(std::memory_order_acquire)) {
                ZRLOG_CPU_PAUSE();
            }
        }
        // 非阻塞尝试加锁：供崩溃抢救路径使用——若锁被垂死线程持有，
        // 强行自旋会永久卡住，导致抢救失败。失败时调用方降级跳过。
        bool try_lock() {
            return !flag_.test_and_set(std::memory_order_acquire);
        }
        void unlock() {
            flag_.clear(std::memory_order_release);
        }
    private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };

    class TscClock {
    public:
        TscClock(const TscClock&) = delete;
        TscClock& operator=(const TscClock&) = delete;

        struct Anchor {
            uint64_t base_ns{ 0 };
            uint64_t base_tsc{ 0 };
        };

        static TscClock& instance() {
            static TscClock t;
            return t;
        }

        static inline uint64_t rdtsc() {
            return zrlog_rdtsc();
        }

        static inline uint64_t now_ns() {
            return instance().current_time_ns();
        }

        static inline uint64_t now_ns_i() {
            return instance().current_time_ns_i();
        }

        inline uint64_t current_time_ns() const {
            uint32_t seq;
            Anchor anc = read_anchor(seq);
            // tsc 在取得一致锚点【之后】读取，保证 tsc >= base_tsc（base_tsc 在该
            // 偶数 seq 发布前已写入），杜绝 tsc-base_tsc 下溢导致的时间戳跳变。
            uint64_t tsc = zrlog_rdtsc();
            return anchor_to_ns(anc.base_ns, anc.base_tsc, tsc);
        }

        // 当前时刻的 ns 时间戳（"i" 表示不保证与系统时钟严格一致，用于低开销场景）。
        // 走与 tsc_to_unix_ns 相同的 seqlock，避免读到被 sync_system_time()
        // 撕裂的 base_ns/base_tsc。
        inline uint64_t current_time_ns_i() const {
            return tsc_to_unix_ns(zrlog_rdtsc());
        }

        // 把前端记录的原始 TSC 计数换算成绝对 Unix 纳秒时间戳（供后端解码使用）。
        // 区别于 tsc2ns()：后者只换算"两个 TSC 之间的差值"，不叠加时间锚点。
        // 用 seqlock 读取锚点（原子量 + fence），与后台 sync_system_time()/calibrate()
        // 的写入互斥，避免读到撕裂的 base_ns/base_tsc。
        inline uint64_t tsc_to_unix_ns(uint64_t tsc) const {
            uint32_t seq;
            Anchor anc = read_anchor(seq);
            return anchor_to_ns(anc.base_ns, anc.base_tsc, tsc);
        }

        // 消费者专用的带版本缓存换算。seq 未变则复用上次锚点，省掉每条日志的
        // seqlock 循环（多次 load + fence）。seq 单调递增，缓存里存的必是偶数
        // 版本，故 seq==cache.seq 时锚点必然一致；写者（calibrate/sync_system_time）
        // 推进 seq 后，下次调用自动走完整 seqlock 重读。供后端解码热路径使用。
        struct AnchorCache {
            uint32_t seq = 0xFFFFFFFFu;  // 初值不等于任何真实偶数 seq，强制首次重读
            Anchor   anc{};
        };
        inline uint64_t tsc_to_unix_ns_cached(uint64_t tsc, AnchorCache& cache) const {
            uint32_t seq = seq_.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(seq != cache.seq)) {
                uint32_t s;
                cache.anc = read_anchor(s);
                cache.seq = s;
            }
            return anchor_to_ns(cache.anc.base_ns, cache.anc.base_tsc, tsc);
        }

        // 每个 TSC tick 对应的纳秒数（用于崩溃路径预算超时，避免信号处理中
        // 调用 steady_clock）。multiplier_ = rate * 2^shift_，其中 rate 即 ns/tick。
        double ns_per_tick() const {
            return static_cast<double>(multiplier_) / static_cast<double>(1ULL << shift_);
        }

        inline uint64_t tsc2ns(uint64_t tsc_diff) const {
#if defined(_MSC_VER) && defined(_M_X64)
            unsigned __int64 high;
            unsigned __int64 low = _umul128(tsc_diff, multiplier_, &high);
            return (high << (64 - shift_)) | (low >> shift_);
#elif defined(__SIZEOF_INT128__)
            return (uint64_t)((unsigned __int128)tsc_diff * multiplier_ >> shift_);
#else
            // 无 128 位乘法的平台（如 MSVC ARM64）：退化为 double 换算。
            // 精度足够——时间戳只需要纳秒级，double 的 53 位尾数在可观测
            // 时长范围内远小于 1ns 误差。
            return static_cast<uint64_t>(tsc_diff * multiplier_d_);
#endif
        }

        bool calibrate(int rounds = 3, int interval_ms = 1) {
            std::vector<double> rates;
            rates.reserve(rounds);

            for (int i = 0; i < rounds; ++i) {
                // 测率用 steady_clock（单调、不受 NTP slew/跳变影响）；绝对墙钟锚点
                // 仍由 sync_anchor_unlocked() 用 system_clock 确定。旧实现用
                // system_clock 测率，NTP 调整期间会把速率算歪。
                auto t0 = std::chrono::steady_clock::now();
                uint64_t tsc0 = zrlog_rdtsc();
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                auto t1 = std::chrono::steady_clock::now();
                uint64_t tsc1 = zrlog_rdtsc();

                auto ns_delta = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                double tsc_delta = static_cast<double>(tsc1 - tsc0);
                if (tsc_delta > 0) {
                    rates.push_back(static_cast<double>(ns_delta) / tsc_delta);
                }
            }

            if (rates.empty()) {
                return false;
            }

            std::sort(rates.begin(), rates.end());
            double rate = rates[rates.size() / 2];

            uint32_t seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_release);

            multiplier_ = static_cast<uint64_t>(rate * (1ULL << shift_));
#if !(defined(_MSC_VER) && defined(_M_X64)) && !defined(__SIZEOF_INT128__)
            // 与 tsc2ns() 的分支保持一致：仅在没有 128 位乘法的平台启用 double 路径
            multiplier_d_ = rate;
#endif
            sync_anchor_unlocked();
            seq_.store(seq + 2, std::memory_order_release);

            return true;
        }

        void sync_system_time() {
            uint32_t seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_release);
            sync_anchor_unlocked();
            seq_.store(seq + 2, std::memory_order_release);
        }

    private:
        TscClock() {
            if (!calibrate()) {
                multiplier_ = 1ULL << shift_;
            }
        }

        // seqlock 读取一致锚点。锚点字段是原子量（relaxed load + acquire fence），
        // 消除普通结构体并发读写的形式数据竞争（TSan 可验证）；seq 奇数或前后不
        // 一致则重试。out_seq 返回读到的偶数版本号，供缓存路径复用。
        inline Anchor read_anchor(uint32_t& out_seq) const {
            uint32_t seq;
            uint64_t bns, btsc;
            do {
                seq = seq_.load(std::memory_order_acquire);
                bns = anchor_base_ns_.load(std::memory_order_relaxed);
                btsc = anchor_base_tsc_.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
            } while (seq != seq_.load(std::memory_order_relaxed) || (seq & 1));
            out_seq = seq;
            return Anchor{ bns, btsc };
        }

        // 锚点 + 原始 TSC -> 绝对 ns。差值按【有符号】解释：前端条目可能记录于
        // 上一次 sync 之前（tsc < base_tsc），无符号相减会下溢成巨值导致时间戳
        // 跳变 ~584 年；有符号解释后正确回退到锚点之前的时刻。
        inline uint64_t anchor_to_ns(uint64_t base_ns, uint64_t base_tsc, uint64_t tsc) const {
            int64_t diff = static_cast<int64_t>(tsc - base_tsc);
            if (diff >= 0) {
                return base_ns + tsc2ns(static_cast<uint64_t>(diff));
            }
            return base_ns - tsc2ns(static_cast<uint64_t>(-diff));
        }

        void sync_anchor_unlocked() {
            auto t = std::chrono::system_clock::now();
            // 调用方已把 seq 置奇（写者临界区），relaxed store 即可，读者由 seqlock 重试。
            anchor_base_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(),
                                  std::memory_order_relaxed);
            anchor_base_tsc_.store(zrlog_rdtsc(), std::memory_order_relaxed);
        }

        static constexpr int shift_ = 32;
        uint64_t multiplier_ = 0;
#if !(defined(_MSC_VER) && defined(_M_X64)) && !defined(__SIZEOF_INT128__)
        double multiplier_d_ = 1.0;
#endif
        std::atomic<uint32_t> seq_{ 0 };
        std::atomic<uint64_t> anchor_base_ns_{ 0 };
        std::atomic<uint64_t> anchor_base_tsc_{ 0 };
    };

    namespace util {
        
        inline uint64_t get_thread_id() {
            static thread_local uint64_t tid_ = []() -> uint64_t {
#ifdef  _WIN32
                return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
                return static_cast<uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
                uint64_t tid;
                pthread_threadid_np(NULL, &tid);
                return tid;
#else
                //return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                static std::atomic<uint64_t> global_tid_{ 0 };
                return global_tid_.fetch_add(1, std::memory_order_relaxed) + 1; // 绝对安全的 fallback
#endif
            }();
            return tid_;
        }

        /**
        * @brief 将当前线程绑定到一组指定的 CPU 核心上
        * @param core_ids 核心 ID 列表，例如 {1, 2, 3}。如果传空，则不限制调度。
        * @return true  绑定成功，或列表为空
        * @return false 绑定失败，或当前系统不支持
        */
        inline bool pin_thread_to_cores(const std::vector<int>& core_ids) {
            if (core_ids.empty()) {
                return true; // 不要求绑核
            }

#if defined(__linux__)
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            // 遍历激活掩码中对应的多个核心位
            for (int core_id : core_ids) {
                if (core_id >= 0) {
                    CPU_SET(core_id, &cpuset);
                }
            }
            int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            return (rc == 0);

#elif defined(_WIN32)
            DWORD_PTR mask = 0;
            // 遍历并进行位或运算，合成最终的多核掩码
            for (int core_id : core_ids) {
                if (core_id >= 0 && core_id < 64) { // Windows 默认 DWORD_PTR 限制 64 核内
                    mask |= (static_cast<DWORD_PTR>(1) << core_id);
                }
            }

            HANDLE thread = GetCurrentThread();
            DWORD_PTR result = SetThreadAffinityMask(thread, mask);
            return (result != 0);
#else
            return false;
#endif
        }

        /**
         * @brief 便利重载：绑定到单个核心
         */
        inline bool pin_thread_to_core(int core_id) {
            if (core_id < 0) {
                return true;
            }
            return pin_thread_to_cores(std::vector<int>{core_id});
        }

        /**
         * @brief 返回性能核（P-core）的逻辑处理器列表；无法判定时返回空。
         *
         * 混合架构（Intel 12 代+ / ARM big.LITTLE）上，把日志后台线程绑到
         * P-core 可避免被调度到 E-core 导致消费吞吐下降。
         *
         * - Windows：读取 GetLogicalProcessorInformationEx 的 EfficiencyClass，
         *   值为 0 视为最高性能核（与实测一致：Ultra 5 125H 上 2-9/16/17 为 0）。
         *   已知限制：只读 GroupMask[0]，即仅覆盖第一个处理器组（<=64 逻辑核）。
         *   >64 核的 Windows 系统会分多个处理器组，跨组绑核需 SetThreadGroupAffinity，
         *   本实现未支持——此类机器上自动绑核可能不完整，建议显式 core_ids 或默认调度。
         * - Linux：/sys/devices/system/cpu/cpuN/cpufreq 或
         *   cpu_capacity 可区分大小核，但各发行版差异大，这里不做推断。
         * - 其他平台或同构架构：返回空，调用方回退到 OS 默认调度。
         */
        inline std::vector<int> get_performance_core_ids() {
            std::vector<int> result;
#if defined(_WIN32)
            // 注意：GetLogicalProcessorInformationEx 是两阶段调用。第一次传
            // nullptr 时必然返回 FALSE（ERROR_INSUFFICIENT_BUFFER）并写出所需
            // 字节数——这是正常流程，不能把返回 FALSE 当成失败。
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            if (len == 0) {
                return result;
            }
            std::vector<BYTE> buf(len);
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
                return result;
            }
            // 先收集每个核的效率等级与掩码，再筛出最小等级（最高性能）
            BYTE best_class = 0xFF;
            const BYTE* p = buf.data();
            const BYTE* end = buf.data() + len;
            while (p < end) {
                auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (info->Size == 0) {
                    break;
                }
                if (info->Relationship == RelationProcessorCore &&
                    info->Processor.EfficiencyClass < best_class) {
                    best_class = info->Processor.EfficiencyClass;
                }
                p += info->Size;
            }
            if (best_class == 0xFF) {
                return result;
            }
            p = buf.data();
            while (p < end) {
                auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (info->Size == 0) {
                    break;
                }
                if (info->Relationship == RelationProcessorCore &&
                    info->Processor.EfficiencyClass == best_class) {
                    const GROUP_AFFINITY& g = info->Processor.GroupMask[0];
                    for (int b = 0; b < 64; ++b) {
                        if (g.Mask & (static_cast<KAFFINITY>(1) << b)) {
                            result.push_back(b);
                        }
                    }
                }
                p += info->Size;
            }
            // 只有一个效率等级 = 同构架构，无需绑核
            if (result.empty()) {
                return result;
            }
#else
            (void)result;
#endif
            return result;
        }

        /**
         * @brief 判定当前是否异构架构（存在多个不同的 EfficiencyClass）。
         * 同构架构上自动绑核没有意义，返回 false 让调用方保持默认调度。
         */
        inline bool has_hybrid_cores() {
#if defined(_WIN32)
            // 两阶段调用，见 get_performance_core_ids 的说明
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            if (len == 0) {
                return false;
            }
            std::vector<BYTE> buf(len);
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
                return false;
            }
            BYTE first = 0xFF;
            bool distinct = false;
            const BYTE* p = buf.data();
            const BYTE* end = buf.data() + len;
            while (p < end) {
                auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (info->Size == 0) {
                    break;
                }
                if (info->Relationship == RelationProcessorCore) {
                    if (first == 0xFF) {
                        first = info->Processor.EfficiencyClass;
                    }
                    else if (info->Processor.EfficiencyClass != first) {
                        distinct = true;
                        break;
                    }
                }
                p += info->Size;
            }
            return distinct;
#else
            return false;
#endif
        }

        // ============================================================================
        // 跨平台大页内存分配器 (HugeTLB Allocator)
        // ============================================================================
        struct MemoryBlock {
            char  *ptr{ nullptr };
            size_t actual_size{ 0 };
            bool   is_huge_page{ false };
        };

        inline MemoryBlock allocate_block(size_t size, bool enable_huge_page = false) {
            MemoryBlock block;
            if (enable_huge_page) {
#if defined(__linux__)
                // Linux 默认大页大小为 2MB。必须按 2MB 向上取整才能分配成功
                const size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
                block.actual_size = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);

                // 尝试分配大页
                void *ptr = ::mmap(nullptr, block.actual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
                if (ptr != MAP_FAILED) {
                    block.ptr = static_cast<char *>(ptr);
                    block.is_huge_page = true;
                    return block;
                }
#elif defined(_WIN32)
                // Windows 大页支持较为复杂，需要 SeLockMemoryPrivilege 权限
                // 这里提供一个尝试机制，失败则退回普通 VirtualAlloc
                const size_t HUGE_PAGE_SIZE = GetLargePageMinimum();
                if (HUGE_PAGE_SIZE > 0) {
                    block.actual_size = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
                    block.ptr = static_cast<char *>(VirtualAlloc(NULL, block.actual_size, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE));
                    if (block.ptr) {
                        block.is_huge_page = true;
                        return block;
                    }
                }
#endif
                // 优雅降级：退回到std::malloc
                block.actual_size = size;
                block.ptr = static_cast<char*>(std::malloc(size));
                return block;
            }

            block.actual_size = size;
            block.ptr = static_cast<char*>(std::malloc(size));
            return block;
        }

        inline void free_block(const MemoryBlock& block) {
            if (!block.ptr) {
                return;
            }
            if (block.is_huge_page) {
#if defined(__linux__)
                ::munmap(block.ptr, block.actual_size);
#elif defined(_WIN32)
                ::VirtualFree(block.ptr, 0, MEM_RELEASE);
#else
                std::free(block.ptr);
#endif
            }
            else {
                std::free(block.ptr);
            }
        }   
    } // namespace util

    enum class LogLevel : uint8_t {
        TRACE = 0, DEBUG, INFO, WARN, ERR, FATAL
    };

    // 1. 使用 constexpr 保证编译期求值，零运行时开销
    // 2. 按值返回 std::string_view
    // 3. 加上 noexcept 关键字，便于编译器优化
    inline constexpr std::string_view loglevel_to_string(LogLevel level) noexcept {
        // 使用 constexpr 数组，完全存储在只读数据段 (.rodata)
        constexpr std::string_view names[] = {
            "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
        };
        //return names[static_cast<uint8_t>(level)];

        // 恢复边界检查，这在日志库中非常关键（防止打印脏数据导致崩溃）
        uint8_t idx = static_cast<uint8_t>(level);
        if (ZRLOG_LIKELY(idx < std::size(names))) {
            return names[idx];
        }
        return "UNKNOWN";
    }

    enum class AppenderType : uint8_t {
        Console,            //控制台
        File,               //文件
        RotatingFile,       //滚动文件
    };

    enum class BufferFullPolicy : uint8_t {
        Discard = 0,        // 策略1：直接丢弃    (默认，保证业务线程绝对不被阻塞，极致低时延)
        Block = 1,          // 策略2：无限重试    (阻塞直到有空间，保证绝对不丢日志，但会导致业务卡顿)
        Retry = 2           // 策略3：有限次重试  (重试一定次数后仍满，则丢弃，兼顾平滑与低时延)
    };

    struct Config {
        AppenderType appender = AppenderType::RotatingFile;
        std::string filename;
        LogLevel level = LogLevel::INFO;
        uint32_t io_buffer_size = 1024 * 1024 * 4;      //io缓冲大小(也即日志格式化缓冲, 全局唯一)
        uint32_t thread_buffer_size = 1024 * 1024 * 1;  //每个线程的缓冲大小(前端二进制序列化缓冲 测试发现越大如16M,时延也变大)
        uint32_t per_thread_quota = 256;                //每个线程的格式化日志的配额(防止线程产生日志太快,公平处理每个线程日志)
        // 前端热路径的可选开销，默认开启。关闭后不再每条日志检查空闲标志，
        // 消费者完全依赖 idle_wait_interval_us 超时轮询，日志落盘延迟上限变为
        // 该间隔（默认 1ms），稀疏日志场景更明显。
        bool     notify_consumer_per_log = true;
        uint32_t idle_wait_interval_us = 1000;          //空闲等待间隔(微秒)，自适应模式下为"空闲态上限"
        // 自适应空闲等待：开启后，有日志流量时把等待间隔压到 idle_busy_interval_us
        // （低延迟，落盘更及时），持续空闲时逐级放大回 idle_wait_interval_us（省 CPU）。
        //
        // 平台默认值不同：
        //  - Linux/macOS：默认开启。nanosleep/condvar 精度足够，50us 就真的睡 50us。
        //  - Windows：默认关闭。std::condition_variable::wait_for 受系统 timer 精度
        //    限制（实测请求 20us 实际睡 ~1.6ms），把间隔压到 50us 没有可测收益，
        //    反而增加唤醒次数。要突破需 timeBeginPeriod(1)（全局副作用）或
        //    CreateWaitableTimerEx 高精度定时器，均不适合作为库的默认行为。
        //    显式设为 true 仍会生效，但效果受上述限制。
#if defined(_WIN32)
        bool     adaptive_idle_wait = false;
#else
        bool     adaptive_idle_wait = true;
#endif
        uint32_t idle_busy_interval_us = 50;            //活跃态等待间隔(微秒)，越小 tail 可见性越好、CPU 占用越高
        // 周期性强制落盘间隔(毫秒)。0 表示关闭周期 fsync：
        //   - 日志仍会在空闲/缓冲满时 write 到 OS（page cache），进程正常退出
        //     时也会 fsync，只是不再每隔 N 毫秒强制刷一次。
        //   - 关闭后断电/内核崩溃最多丢失 page cache 中的未刷数据。
        // 适用场景：部署在 SSD/tmpfs 且追求后端吞吐（实测本机 4MB flush 约
        // 4.1ms，其中 write 1.3ms + fsync 2.8ms，占缓冲填满间隔约 3%）。
        // 慢盘/网络存储应保持默认值，因为单次 fsync 可达数十毫秒。
        uint32_t force_flush_interval_ms = 3000;        //强制刷盘间隔(毫秒)，0=关闭周期 fsync
        uint32_t tsc_sync_interval_s = 3600;            //Tsc时钟同步间隔(秒) (默认:1小时 既能消除长期漂移，又避免被 NTP 频繁干扰)
        uint32_t crash_drain_wait_ms = 3000;            //发生崩溃的业务线程自旋阻塞，给后台线程抢救数据的时长(毫秒)
        bool     enable_huge_page = false;              //启用OS的HugePage内存分配

        // 支持将日志后台线程绑定到多个核心 (如 {0, 1, 2})
        // 默认空列表表示由 OS 全局调度
        std::vector<int> background_thread_core_ids;

        // 自动把后台线程绑到性能核（P-core）。仅 Windows 生效（读取
        // GetLogicalProcessorInformationEx 的 EfficiencyClass）。
        // opt-in：混合架构（如 Intel 12 代+）上若后台线程被调度到 E-core，
        // 消费吞吐会显著下降；但自动绑核可能与业务线程争抢 P-core，
        // 故默认关闭，由使用者按场景决定。显式设置 background_thread_core_ids
        // 时以显式列表为准，本开关不生效。
        bool     auto_pin_to_p_cores = false;

        // ---- 缓冲区满时的处理策略 ----
        BufferFullPolicy buffer_full_policy = BufferFullPolicy::Discard;
        uint32_t buffer_full_retry_count = 256;             //重试次数(仅在 Retry 策略下生效)

        // ---- 滚动文件配置 ----
        uint32_t rotating_file_size = 1024 * 1024 * 100;    // 默认 100MB 滚动一次
        uint32_t rotating_max_files = 5;                    // 默认保留 5 个备份文件
    };

    class ILogAppender {
    public:
        virtual ~ILogAppender() = default;

        int writen(const char* data, size_t len) {
            const char* ptr = data;
            size_t nleft = len;

            while (nleft > 0) {
                int ret = write(ptr, nleft);
                if (ret > 0) {
                    nleft -= static_cast<size_t>(ret);
                    ptr += ret;
                    continue;
                }
                if (ret < 0 && errno == EINTR) {
                    continue;  // 被信号打断，重试是安全的
                }
                // EAGAIN/EWOULDBLOCK 表示 fd 暂时不可写。旧实现直接 continue，
                // 会在非阻塞 fd 上无限忙等并占满消费线程。这里改为放弃剩余部分，
                // 让调用方（IOBuffer）按已写字节数处理。
                break;
            }

            return static_cast<int>(len - nleft);
        }

        virtual int write(const char* data, size_t len) = 0;
        virtual int flush() = 0;
    };

    class FileLogAppender : public ILogAppender {
    public:
        FileLogAppender(const std::string& path) {
            fd_ = ZRLOG_OPEN(path.c_str(), ZRLOG_O_FLAGS, ZRLOG_S_FLAGS);
        }

        ~FileLogAppender() override {
            close();
        }

        bool is_open() const {
            return fd_ != -1;
        }

        int write(const char* data, size_t len) override {
            return ZRLOG_WRITE(fd_, data, len);
        }

        int flush() override {
            return ZRLOG_FLUSH_FILE(fd_);
        }

        void close() {
            if (fd_ != -1) {
                ZRLOG_CLOSE(fd_);
                fd_ = -1;
            }
        }

    private:
        int fd_{ -1 };
    };

    class RotatingFileLogAppender : public ILogAppender {
    public:
        RotatingFileLogAppender(const std::string& path, size_t max_size, uint32_t max_files)
            : base_path_(path), max_size_(max_size), max_files_(max_files), next_roll_size_(max_size) {

            // 1. 扫描历史日志目录，确定下一个滚动序号，并清理残留的孤儿文件
            if (max_files_ > 0) {
                init_next_file_index();
            }

            // 2. 打开当前的活跃文件 (永远是 base_path_)
            open_current_file();
        }

        ~RotatingFileLogAppender() override {
            close();
        }

        int write(const char* data, size_t len) override {
            // 触发器：正常写满，或在退避期写够了数据
            if (ZRLOG_UNLIKELY(current_size_ + len > next_roll_size_)) {
                roll();
            }

            // 兜底 1：文件彻底瘫痪，全量降级到 stderr
            if (ZRLOG_UNLIKELY(fd_ == -1)) {
                int ret = ZRLOG_WRITE(STDERR_FILENO, data, len);
                if (ret > 0) {
                    current_size_ += ret; // 累加字节数作为重试定时器
                }
                return ret;
            }

            // 正常写入
            int ret = write_current_fd(data, len);
            if (ret > 0) {
                current_size_ += ret;
                return ret;
            }

            // 立即保存 errno：下面的 stderr 兜底写若也失败（如 stderr 被重定向到
            // 满管道 → EPIPE）会改写 errno，导致硬/软错误分类被污染。
            const int err = errno;

            // 写失败（如 ENOSPC 磁盘满）。先把数据兜底到 stderr，避免静默丢失。
            [[maybe_unused]] auto sres = ZRLOG_WRITE(STDERR_FILENO, data, len);

            // 关键修复（issue #8）：未写入文件的字节绝不能累加进 current_size_。
            // 旧实现 `current_size_ += len` 会虚增文件大小，持续失败时迅速越过
            // next_roll_size_ 触发 roll()，形成 rename/open 风暴。
            // 软错误（磁盘满等）下 fd 仍有效，保持大小不变，等空间释放后自然恢复。
            // 硬错误（fd 已不可用）则关闭 fd，交给上面的 fd_==-1 兜底路径，
            // 由它按 stderr 实际字节数推进重试计时器，避免在死描述符上反复失败。
            if (is_hard_write_error(err)) {
                close();
                current_size_ = 0;
                size_t retry_interval = max_size_ / 10;
                if (retry_interval < 1024 * 1024) {
                    retry_interval = 1024 * 1024;
                }
                next_roll_size_ = retry_interval;
            }
            return ret;
        }

        int flush() override {
            if (ZRLOG_LIKELY(fd_ != -1)) {
                return ZRLOG_FLUSH_FILE(fd_);
            }
            return 0;
        }

        // 只读观测接口：用于测试与监控轮转状态，不改变任何行为。
        size_t current_size() const { 
            return current_size_; 
        }
        bool   is_open() const { 
            return fd_ != -1; 
        }

        // 区分"文件描述符本身已不可用"的硬错误与"暂时写不进去"的软错误。
        // 硬错误关闭 fd 交给 stderr 兜底；软错误（磁盘满、配额）保留 fd，
        // 等外部条件恢复后自动继续写入同一文件。公开以便单测覆盖分类逻辑。
        static bool is_hard_write_error(int err) {
            switch (err) {
#if defined(EBADF)
            case EBADF:   // 描述符无效
#endif
#if defined(EFBIG)
            case EFBIG:   // 超出文件大小限制
#endif
#if defined(EPIPE)
            case EPIPE:   // 管道断开
#endif
#if defined(EIO)
            case EIO:     // 底层 I/O 错误
#endif
                return true;
            default:
                return false;
            }
        }

    private:
        // 写满整个 buffer（处理短写与 EINTR）。返回实际写入字节数，失败返回 -1。
        // 旧实现只调用一次 write()，短写时剩余数据被静默丢弃，同时 current_size_
        // 只累加了部分字节，导致轮转阈值判断与实际文件大小脱节。
        int write_current_fd(const char* data, size_t len) {
            size_t written = 0;
            while (written < len) {
                int ret = ZRLOG_WRITE(fd_, data + written, len - written);
                if (ret > 0) {
                    written += static_cast<size_t>(ret);
                    continue;
                }
                if (ret < 0 && errno == EINTR) {
                    continue;
                }
                if (ret < 0 && errno == EAGAIN) {
                    // 非阻塞 fd 暂时不可写：已写入的部分保留，剩下的放弃
                    break;
                }
                if (written == 0) {
                    return -1;  // 一个字节都没写进去，交给调用方按错误处理
                }
                break;  // 部分写入后失败，按已写入量返回
            }
            return static_cast<int>(written);
        }

        void init_next_file_index() {
            next_file_idx_ = 1;
            try {
                std::filesystem::path p(base_path_);
                auto dir = p.parent_path();
                if (dir.empty()) {
                    dir = ".";
                }
                std::string filename = p.filename().string();

                std::vector<uint64_t> existing_indices;
                std::error_code ec;

                // 遍历同级目录，找出形如 "app.log.123" 的所有备份文件
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (entry.is_regular_file(ec)) {
                        std::string name = entry.path().filename().string();
                        // 匹配前缀 "app.log."
                        if (name.length() > filename.length() + 1 &&
                            name.compare(0, filename.length() + 1, filename + ".") == 0) {
                            const char* num_str = name.c_str() + filename.length() + 1;
                            char* end_ptr = nullptr;
                            uint64_t idx = std::strtoull(num_str, &end_ptr, 10);
                            if (*end_ptr == '\0') {
                                existing_indices.push_back(idx);
                            }
                        }
                    }
                }

                if (!existing_indices.empty()) {
                    std::sort(existing_indices.begin(), existing_indices.end());
                    // 下一个将要被滚动的后缀，必然是当前存在的最大序号 + 1
                    next_file_idx_ = existing_indices.back() + 1;

                    // 【鲁棒性修复：清理孤儿文件】
                    // 防止程序之前在 max_files=10 时运行，后来配置改成了 max_files=3，导致旧文件永远泄漏
                    if (existing_indices.size() > max_files_) {
                        size_t to_delete = existing_indices.size() - max_files_;
                        char old_path[4096];
                        for (size_t i = 0; i < to_delete; ++i) {
                            auto res = fmt::format_to_n(old_path, sizeof(old_path) - 1, "{}.{}", base_path_, existing_indices[i]);
                            old_path[std::min<size_t>(res.size, sizeof(old_path) - 1)] = '\0';
                            std::remove(old_path);
                        }
                    }
                }
            }
            catch (...) {
                // 若发生无权限等文件系统异常，优雅降级，默认序号从 1 开始
            }
        }

        void open_current_file() {
            // 永远打开固定的 base_path_ (例如 app.log)
            fd_ = ZRLOG_OPEN(base_path_.c_str(), ZRLOG_O_FLAGS, ZRLOG_S_FLAGS);
            if (fd_ != -1) {
                struct stat st;
                if (fstat(fd_, &st) == 0) {
                    current_size_ = st.st_size; // 如果进程重启，继承现存 app.log 的大小，继续追加
                }
                else {
                    current_size_ = 0;
                }
                next_roll_size_ = max_size_;
            }
            else {
                // 文件打开失败的严重报警与退避设定
                const char err_msg[] = "\n[ZRLOG CRITICAL] Failed to open log file! Fallback to stderr.\n";
                [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg) - 1);
                current_size_ = 0;

                size_t retry_interval = max_size_ / 10;
                if (retry_interval < 1024 * 1024) {
                    retry_interval = 1024 * 1024;
                }
                next_roll_size_ = retry_interval;
            }
        }

        void close() {
            if (fd_ != -1) {
                ZRLOG_CLOSE(fd_);
                fd_ = -1;
            }
        }

        void roll() {
            close();
            bool rename_success = true;

            if (max_files_ > 0) {
                if (ZRLOG_ACCESS(base_path_.c_str(), ZRLOG_F_OK) == 0) {

                    // ==========================================================
                    // 核心 O(1) 滑动窗口滚动逻辑：仅 1 次 Rename，1 次 Remove
                    // ==========================================================
                    char backup_path[4096];
                    auto res = fmt::format_to_n(backup_path, sizeof(backup_path) - 1, "{}.{}", base_path_, next_file_idx_);
                    // 严格边界检查：防止格式化超长引发栈溢出
                    backup_path[std::min<size_t>(res.size, sizeof(backup_path) - 1)] = '\0';

                    std::remove(backup_path); // 极小概率冲突下的防御性清除

                    // 1. 将 app.log 重命名为 app.log.N (N 递增不回头)
                    if (std::rename(base_path_.c_str(), backup_path) != 0) {
                        rename_success = false;
                        const char err_msg[] = "\n[ZRLOG ERROR] Rotate rename failed!\n";
                        [[maybe_unused]] auto w_res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg) - 1);
                    }
                    else {
                        // 2. 淘汰超出滑动窗口最左端的老文件
                        // 若 max_files = 3，当前给备份命名的序号是 6，说明应该删除 6 - 3 = 3
                        if (next_file_idx_ > max_files_) {
                            char old_path[4096];
                            auto old_res = fmt::format_to_n(old_path, sizeof(old_path) - 1, "{}.{}", base_path_, next_file_idx_ - max_files_);
                            old_path[std::min<size_t>(old_res.size, sizeof(old_path) - 1)] = '\0';
                            std::remove(old_path);
                        }
                        // 序号推进
                        ++next_file_idx_;
                    }
                }
            }
            else {
                // 如果 max_files == 0，即不保留任何备份，则直接删除旧日志重新开始
                std::remove(base_path_.c_str());
            }

            // 重新创建全新的 app.log 并分配新的 Inode
            open_current_file();

            // 异常退避策略：若重命名失败，延长重试间距以防疯狂拉起 I/O 导致磁盘 I/O 挂起
            if (max_files_ > 0 && !rename_success && fd_ != -1) {
                size_t backoff_step = max_size_ / 10;
                if (backoff_step < 1024 * 1024) {
                    backoff_step = 1024 * 1024;
                }
                next_roll_size_ = current_size_ + backoff_step;
            }
        }

        std::string base_path_;
        size_t max_size_;
        uint32_t max_files_;
        size_t current_size_{ 0 };
        size_t next_roll_size_;
        int fd_{ -1 };

        uint64_t next_file_idx_{ 1 }; // uint64_t 保证无穷滚动，标识下一个生成的备份后缀
    };

    class ConsoleLogAppender : public ILogAppender {
    public:
        int write(const char* data, size_t len) override {
            return ZRLOG_WRITE(STDOUT_FILENO, data, len);
        }

        int flush() override {
            return ZRLOG_FLUSH_FILE(STDOUT_FILENO);
        }
    };

    class NanoLogger {
    public:
        // 极限前端的编译期策略。默认完全关闭，不改变现有运行时配置语义。
        // benchmark 可定义 ZRLOG_COMPILE_TIME_POLICY 验证：确定使用 Discard
        // 且不逐条通知时，让 MSVC 删除热函数中永远不会执行的策略分支。
#ifdef ZRLOG_COMPILE_TIME_POLICY
        static constexpr bool compile_time_policy = true;
        static constexpr BufferFullPolicy frontend_full_policy = BufferFullPolicy::Discard;
        static constexpr bool frontend_notify_per_log = false;
#else
        static constexpr bool compile_time_policy = false;
        static constexpr BufferFullPolicy frontend_full_policy = BufferFullPolicy::Discard;
        static constexpr bool frontend_notify_per_log = true;
#endif

        class IOBuffer;
        // ThreadBuffer 是内部实现，前置声明放在 private 区（与定义同区，
        // 否则 GCC 报 "redeclared with different access"；MSVC 容忍该写法）。
        struct LogMeta;
        struct LogEntryHeader;
        using DecoderFn = void(*)(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out);

        struct LogMeta {
            uint32_t    id = 0;
            LogLevel    level = LogLevel::TRACE;
            uint32_t    line = 0;
            std::string_view file;
            std::string_view func;
            std::string format;
            DecoderFn   decoder;
        };

        //【极限压缩】将 LogEntryHeader 压缩至 16 字节，大幅提升缓存命中率与容量
        // tsc 存的是前端读到的原始 TSC 计数（不是 ns）：前端只做 rdtsc，
        // 把 TSC->ns 的换算与锚点读取全部推迟到后端解码时完成。
        // 前提是 invariant TSC（跨核同步），已实测 P 核间偏移 <=1 tick。
        // 16 字节 header：tag 合进 size_tag 高 8 位（[31:24]=tag, [23:0]=size），
        // meta_id 为数组索引（0=PADDING, 1+=valid）。前端零额外开销。
        static constexpr uint32_t kTagStatic  = 0;
        static constexpr uint32_t kTagDyn     = 1;
        static constexpr uint32_t kTagPadding = 2;
        static constexpr uint32_t kSizeMask   = 0x00FFFFFF;
        static constexpr uint32_t kTagShift   = 24;
        struct LogEntryHeader {
            uint32_t size_tag;
            uint32_t meta_id;
            uint64_t tsc;
        };
        static_assert(sizeof(LogEntryHeader) == 16, "LogEntryHeader layout changed");

        class IOBuffer {
        public:
            IOBuffer(ILogAppender* appender, uint32_t size, bool enable_huge_page = false)
                : appender_(appender), size_(size) {
                if (size_ < 1024) {
                    size_ = 1024;
                }
                // 大页可显著减少解码热循环的 TLB miss（4MB 缓冲 = 2 个 2MB 大页）。
                // allocate_block 内部已含普通内存降级；若仍失败（极端 OOM），用
                // vector 兜底保证 data_ 非空——decoder 依赖 flush 后可用量 >= 1。
                mem_block_ = util::allocate_block(size_, enable_huge_page);
                if (mem_block_.ptr) {
                    data_ = mem_block_.ptr;
                    if (!mem_block_.is_huge_page) {
                        volatile char* p = data_;
                        for (size_t i = 0; i < size_; i += 4096) {
                            p[i] = 0;  // 预热按页映射，避免解码时缺页
                        }
                    }
                }
                else {
                    fallback_.resize(size_);
                    data_ = fallback_.data();
                }
            }

            ~IOBuffer() {
                util::free_block(mem_block_);
            }

            IOBuffer(const IOBuffer&) = delete;
            IOBuffer& operator=(const IOBuffer&) = delete;

            void append(const char* src, size_t len) {
                if (ZRLOG_UNLIKELY(pos_ + len > size_)) {
                    flush_to_os();
                }
                if (ZRLOG_UNLIKELY(len > size_)) {
                    int w = appender_->writen(src, len);
                    size_t done = (w > 0) ? static_cast<size_t>(w) : 0;
                    if (done < len) {
                        io_drop_bytes_ref().fetch_add(len - done, std::memory_order_relaxed);
                    }
                    return;
                }
                std::memcpy(data_ + pos_, src, len);
                pos_ += len;
            }

            void flush_to_os() {
                if (ZRLOG_LIKELY(pos_ == 0)) {
                    return;
                }
                int written = appender_->writen(data_, pos_);
                size_t done = (written > 0) ? static_cast<size_t>(written) : 0;
                if (ZRLOG_LIKELY(done == pos_)) {
                    pos_ = 0;
                    return;
                }
                // 部分写/失败（如 ENOSPC）：旧实现直接 pos_=0 会静默丢弃未写 tail。
                // 现保留未写部分供下次 flush 重试，但保留量不得超过缓冲一半——
                // 否则持续失败时旧数据占满缓冲，decoder 可用空间趋零会引发越界。
                // 超限部分计入丢弃统计（io_drop_bytes()），保证 flush 后可用量 >= size_/2。
                size_t remaining = pos_ - done;
                if (remaining > size_ / 2) {
                    io_drop_bytes_ref().fetch_add(remaining, std::memory_order_relaxed);
                    pos_ = 0;
                }
                else {
                    std::memmove(data_, data_ + done, remaining);
                    pos_ = remaining;
                }
            }

            void flush_force() {
                flush_to_os();
                appender_->flush();
            }

            char* current_ptr() {
                return data_ + pos_;
            }
            size_t available_size() const {
                return size_ - pos_;
            }
            void advance(size_t len) {
                pos_ += len;
            }

        private:
            ILogAppender* appender_ = nullptr;
            util::MemoryBlock mem_block_{};
            std::vector<char> fallback_;  // 仅极端 OOM 时使用，正常为空
            char* data_ = nullptr;
            size_t size_;
            size_t pos_ = 0;
        };

        static NanoLogger& instance() {
            static NanoLogger logger;
            return logger;
        }

        bool init(const Config& config) {
            // 约定：init 只在启动 logging 之前调用一次。运行中重复 init 为无操作
            // （幂等），避免改写 config_ 与后台线程读 config_、前端读缓冲配置构成
            // 数据竞争。需重新配置请先 fini() 再 init()；运行时调级用 set_level()。
            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                if constexpr (compile_time_policy) {
                    // 编译期策略承诺与运行时 Config 一致（见类头注释），不一致
                    // 直接拒绝启动：静默忽略会按编译期分支丢日志，极难排查。
                    if (config.buffer_full_policy != frontend_full_policy ||
                        config.notify_consumer_per_log != frontend_notify_per_log) {
                        return false;
                    }
                }
                config_ = config;
                hot_state_.store(
                    static_cast<uint32_t>(config_.level) |
                    (config_.notify_consumer_per_log ? kHotNotifyBit : 0u),
                    std::memory_order_relaxed);

                if (config_.appender == AppenderType::File) {
                    appender_ = std::make_unique<FileLogAppender>(config_.filename);
                }
                else if (config_.appender == AppenderType::RotatingFile) {
                    appender_ = std::make_unique<RotatingFileLogAppender>(config_.filename, 
                        config_.rotating_file_size, config_.rotating_max_files);
                }
                else {
                    appender_ = std::make_unique<ConsoleLogAppender>();
                }

                log_thread_running_.store(true, std::memory_order_relaxed);
                log_thread_ = std::thread(&NanoLogger::poll_routine, this);
            }
            return true;
        }

        bool init(const std::string& filename, LogLevel level) {
            Config cfg;
            cfg.filename = filename;
            cfg.level = level;
            cfg.appender = AppenderType::File;
            return init(cfg);
        }

        void fini() {
            if (log_thread_running_.load(std::memory_order_relaxed)) {
                log_thread_running_.store(false, std::memory_order_relaxed);
                if (log_thread_.joinable()) {
                    // 持锁 notify：与本 cv 的协议保持一致（裸 notify 在消费者未进入 wait 时会丢失，
                    // 导致 shutdown 最多多等一个 idle 超时）
                    {
                        std::lock_guard<std::mutex> lock(idle_wait_mutex_);
                        idle_wait_condition_.notify_one();
                    }
                    log_thread_.join();
                }
                appender_.reset();
            }
        }

        inline bool check_level(LogLevel level) const {
            // 读打包热状态而非 config_.level：消除与 init() 写 config_ 的数据竞争，
            // 并支持运行时调级（relaxed load 在 x86 上与普通 load 同价）。
            return level >= static_cast<LogLevel>(
                hot_state_.load(std::memory_order_relaxed) & kHotLevelMask);
        }

        // 运行时调整日志级别（线程安全，立即生效）。
        inline void set_level(LogLevel level) {
            // CAS 更新级别位，保留通知位不变。
            uint32_t old = hot_state_.load(std::memory_order_relaxed);
            uint32_t val;
            do {
                val = (old & ~kHotLevelMask) | static_cast<uint32_t>(level);
            } while (!hot_state_.compare_exchange_weak(
                old, val, std::memory_order_relaxed, std::memory_order_relaxed));
        }

        // ========================================================================
        // Frontend: 静态日志入口 (通过 FMT_COMPILE 实现极致前端序列化)
        // ========================================================================
        template<typename FmtProvider, typename... Args>
        bool log(std::atomic<uint32_t>& log_id_atom, LogLevel level, std::string_view file, uint32_t line,
            std::string_view func, Args&&... args) {

            // 在编译期扫描所有参数，如果有裸指针，直接让编译失败！
            static_assert((... && !detail::is_raw_char_ptr_v<Args>),
                "[ZRLOG FATAL ERROR] Raw char* / const char* is strictly forbidden for performance and safety! "
                "Please use std::string_view(ptr, len) or zrlog::literal instead.");

            // 参数必须可按值安全拷贝（前端 memcpy 进环形缓冲、后端 memcpy 出来）。
            // 含堆内存的类型（如内嵌 std::string 的自定义结构体）逐位拷贝后会在
            // 后端形成悬垂指针。若确需支持，为该类型特化 detail::loggable_arg。
            static_assert((... && detail::loggable_arg<Args>::value),
                "[ZRLOG FATAL ERROR] Log argument is not safely copyable by value. "
                "Only trivially copyable types, std::string, std::string_view and "
                "zrlog::string_literal_t are supported. Wrap heap-owning types "
                "(e.g. nested std::string) into a plain struct/array, or specialize "
                "zrlog::detail::loggable_arg<T> to std::true_type with your own guarantee.");

            // 一次 load 同时获取级别与通知开关（见 hot_state_ 注释）。
            const uint32_t hs = hot_state_.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(level < static_cast<LogLevel>(hs & kHotLevelMask))) {
                return false;
            }

            uint32_t log_id = log_id_atom.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(0 == log_id)) {
                log_id = register_log_meta<FmtProvider, typename std::decay<Args>::type...>(
                    log_id_atom, level, file, line, func);
            }

            return push_log_entry(hs, kTagStatic, log_id, std::forward<Args>(args)...);
        }

        // ========================================================================
        // Frontend: 动态日志入口 (支持运行时 std::string format)
        // ========================================================================
        template<typename... Args>
        bool log_runtime(std::atomic<uint64_t>& dyn_state,
            LogLevel level, std::string_view file, uint32_t line,
            std::string_view func, std::string_view format, Args&&... args) {

            // 在编译期扫描所有参数，如果有裸指针，直接让编译失败！
            static_assert((... && !detail::is_raw_char_ptr_v<Args>),
                "[ZRLOG FATAL ERROR] Raw char* / const char* is strictly forbidden for performance and safety! "
                "Please use std::string_view(ptr, len) or zrlog::literal instead.");

            // 同 log()：参数必须可按值安全拷贝（见 detail::loggable_arg 注释）。
            static_assert((... && detail::loggable_arg<Args>::value),
                "[ZRLOG FATAL ERROR] Log argument is not safely copyable by value. "
                "Only trivially copyable types, std::string, std::string_view and "
                "zrlog::string_literal_t are supported. See zrlog::detail::loggable_arg.");

            // DYN 语义：格式串允许运行时变化。快路径用内容哈希做无锁校验，
            // 首次调用或哈希不一致才进入锁内重注册。
            //
            // 关键：log_id 与哈希必须打包进【单个】原子量（高 32 位 id | 低 32 位
            // 哈希），一次 load 拿到一致快照。旧实现用两个独立原子量分别 store，
            // 两线程同调用点并发换格式串时可交错成 "id 指向 F2、缓存哈希却是 F1"，
            // 之后所有 F1 调用哈希命中却按 F2 渲染，错配【持续存在】直到某次 F2
            // 调用才自愈。打包后该交错不可能发生。
            // 取舍：哈希截断为 32 位，同调用点两个不同格式串低 32 位碰撞概率 ~2^-32，
            // 碰撞时快路径误命中按旧格式渲染一条；慢路径仍按全内容查重兜底。
            uint64_t fmt_hash = detail::fnv1a_hash(format);
            uint32_t hash_lo = static_cast<uint32_t>(fmt_hash);
            uint64_t state = dyn_state.load(std::memory_order_relaxed);
            uint32_t log_id = static_cast<uint32_t>(state >> 32);
            if (ZRLOG_UNLIKELY(0 == log_id || hash_lo != static_cast<uint32_t>(state))) {
                log_id = register_runtime_log_meta<typename std::decay<Args>::type...>(
                    level, file, line, func, format);
                dyn_state.store((static_cast<uint64_t>(log_id) << 32) | hash_lo,
                                std::memory_order_relaxed);
            }

            const uint32_t hs = hot_state_.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(level < static_cast<LogLevel>(hs & kHotLevelMask))) {
                return false;
            }

            return push_log_entry(hs, kTagDyn, log_id, std::forward<Args>(args)...);
        }

        // 崩溃路径预热：在正常上下文缓存 ns/tick，供 emergency_flush() 在
        // 信号处理器内预算超时使用（信号处理中不可调用 steady_clock）。
        void cache_crash_clock() {
            crash_ns_per_tick_ = TscClock::instance().ns_per_tick();
        }

        // 崩溃兜底紧急刷盘：可在信号处理器中安全调用。
        // 只做 async-signal-safe 的动作：原子标志 + rdtsc 忙等，绝不调用
        // sleep_for / steady_clock / instance()（后三者会加锁或非信号安全）。
        void emergency_flush() {
            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                return;
            }

            // 标记崩溃模式，后台线程每轮循环都会检查此标志并立即全量 drain
            is_crashed_.store(true, std::memory_order_release);

            // 用 rdtsc 计算超时，避免 steady_clock（可能走系统调用）与 sleep_for。
            // ns_per_tick 由 install_crash_handler() 在正常上下文预先缓存。
            const double ns_per_tick =
                crash_ns_per_tick_ > 0 ? crash_ns_per_tick_ : 1.0;
            const uint64_t wait_ticks = static_cast<uint64_t>(
                static_cast<double>(config_.crash_drain_wait_ms) * 1e6 / ns_per_tick);
            const uint64_t deadline = TscClock::rdtsc() + wait_ticks;

            while (!crash_drain_done_.load(std::memory_order_acquire)) {
                if (TscClock::rdtsc() > deadline) {
                    break;
                }
                ZRLOG_CPU_PAUSE();
            }
        }

    private:

        class ThreadBuffer;

        // ========================================================================
        // 公共的入队与序列化逻辑 (被 log 和 log_runtime 共同调用，完美转发)
        // ========================================================================
        // 满缓冲慢路径（Block/Retry 自旋）拆到 noinline 冷函数；Discard 仍在
        // 调用点原地返回，因为压测与丢日志模式下"满"是正常路径，不能多付调用。
        // 这段慢代码体积大（两个 do-while + yield + config_ 读取），内联展开在
        // push_log_entry 里会抬高快路径的寄存器压力与栈帧（TLS 拆分实验已证明
        // MSVC 的帧成本是真实延迟来源）。快路径只剩"alloc 成功 -> 序列化 -> commit"。
        ZRLOG_NOINLINE char* handle_full_buffer(ThreadBuffer* buffer, uint32_t aligned_len) {
            if constexpr (compile_time_policy) {
                // 编译期锁定满策略（当前仅支持 Discard，见类头常量声明）：
                // 无 switch、无 config_ 读取，满即丢。定义
                // ZRLOG_COMPILE_TIME_POLICY 即承诺运行时 Config 与此一致。
                return nullptr;
            }
            switch (config_.buffer_full_policy) {
            case BufferFullPolicy::Discard:
                return nullptr;

            case BufferFullPolicy::Block: {
                notify_consumer();
                // 修复：单条日志超过整个线程缓冲容量时永远无法入队（即使缓冲全空也放不下），
                // 直接放弃，避免业务线程在此永久自旋（活锁）直到 fini()
                if (aligned_len >= buffer->capacity()) {
                    return nullptr;
                }
                uint32_t spin_count = 0;
                do {
                    if (!log_thread_running_.load(std::memory_order_relaxed)) {
                        return nullptr;
                    }
                    if (spin_count < 256) {
                        ZRLOG_CPU_PAUSE();
                    }
                    else {
                        std::this_thread::yield();
                    }
                    ++spin_count;
                    char* p = buffer->alloc(aligned_len);
                    if (p != nullptr) return p;
                } while (true);
            }

            case BufferFullPolicy::Retry: {
                notify_consumer();
                // 同 Block：容量不够的条目永远放不下，快速放弃而非空转 256 次重试
                if (aligned_len >= buffer->capacity()) {
                    return nullptr;
                }
                uint32_t spin_count = 0;
                do {
                    if (!log_thread_running_.load(std::memory_order_relaxed)) {
                        return nullptr;
                    }
                    if (spin_count >= config_.buffer_full_retry_count) {
                        return nullptr;
                    }
                    if (spin_count < 256) {
                        ZRLOG_CPU_PAUSE();
                    }
                    else {
                        std::this_thread::yield();
                    }
                    ++spin_count;
                    char* p = buffer->alloc(aligned_len);
                    if (p != nullptr) return p;
                } while (true);
            }

            default:
                return nullptr;
            }
        }

        template<typename... Args>
        inline bool push_log_entry(uint32_t hot_state, uint32_t tag, uint64_t meta_or_id, Args&&... args) {
            ThreadBuffer *buffer = get_thread_buffer();
            if (ZRLOG_UNLIKELY(nullptr == buffer)) {
                return false;
            }
            uint32_t total_size = sizeof(LogEntryHeader) + calculate_args_size_all(args...);
            //为了内存地址对齐,所以将总长度向上对齐到 8 字节！
            uint32_t aligned_len = (total_size + 7) & ~7u;

            // rdtsc 前置到 alloc() 之前：rdtsc 有 12-16 周期延迟，放在 alloc
            // 返回之后（旧行为）会串行在关键路径上；前置后可与 call setup +
            // alloc 函数体重叠。quill 的 log_statement 同样在队列预留之前取时间戳。
            // 语义不变：仍记录"发起写入"时刻，与 quill 一致。
            const uint64_t tsc = TscClock::rdtsc();

            char *ptr = buffer->alloc(aligned_len);
            if (ZRLOG_UNLIKELY(nullptr == ptr)) {
                if constexpr (compile_time_policy) {
                    return false;
                }
                else {
                    // Discard 是压测与丢日志模式下的正常满路径，必须原地返回，
                    // 不能为了一次永远失败的分配多付一次冷函数调用。
                    if (ZRLOG_LIKELY(config_.buffer_full_policy == BufferFullPolicy::Discard)) {
                        return false;
                    }
                    ptr = handle_full_buffer(buffer, aligned_len);
                    if (nullptr == ptr) {
                        return false;
                    }
                }
            }

            // 分配成功，进行真正的极速序列化
            LogEntryHeader *header = reinterpret_cast<LogEntryHeader *>(ptr);
            header->size_tag = (tag << kTagShift) | (aligned_len & kSizeMask);
            header->meta_id = static_cast<uint32_t>(meta_or_id);
            header->tsc = tsc;
            serialize_args_all(ptr + sizeof(LogEntryHeader), std::forward<Args>(args)...);
            buffer->commit(aligned_len);

            if constexpr (compile_time_policy) {
                // 编译期锁定"不逐条通知"：删除热函数里的 config_ 读取与分支。
                // benchmark 用它测纯序列化+入队下限；消费者靠 idle 超时轮询。
            }
            else {
                if (ZRLOG_LIKELY(hot_state & kHotNotifyBit)) {
                    notify_consumer();
                }
            }

            return true;
        }

        // ------------------------------------------------------------------------
        // 极速前端序列化引擎 (零拷贝、无 \0 冗余)
        // ------------------------------------------------------------------------
        template <typename T>
        static constexpr uint32_t arg_size(const T& val) noexcept {
            using DecayedT = std::decay_t<T>;
            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                return sizeof(zrlog::string_literal_t);
            }
            else if constexpr (std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                return sizeof(uint32_t) + (uint32_t)val.size();
            }
            else {
                return sizeof(T);
            }
        }

        template <size_t N>
        static constexpr uint32_t arg_size(const char(&)[N]) noexcept {
            return sizeof(uint32_t) + (N > 0 ? static_cast<uint32_t>(N - 1) : 0);
        }

        static constexpr uint32_t calculate_args_size_all() noexcept {
            return 0;
        }

        template <typename... Args>
        //static constexpr uint32_t calculate_args_size_all(const Args&... args) noexcept {
        static uint32_t calculate_args_size_all(const Args&... args) noexcept {
            return (0 + ... + arg_size(args));
        }

        template <typename T>
        static void serialize_arg(char*& ptr, const T& val) {
            using DecayedT = std::decay_t<T>;

            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                std::memcpy(ptr, &val, sizeof(zrlog::string_literal_t));
                ptr += sizeof(zrlog::string_literal_t);
            }
            else if constexpr (std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                uint32_t len = (uint32_t)val.size();
                std::memcpy(ptr, &len, sizeof(uint32_t));
                if (len > 0) {
                    std::memcpy(ptr + sizeof(uint32_t), val.data(), len);
                }
                ptr += sizeof(uint32_t) + len;
            }
            else {
                std::memcpy(ptr, &val, sizeof(T));
                ptr += sizeof(T);
            }
        }

        template <size_t N>
        static void serialize_arg(char*& ptr, const char(&val)[N]) {
            uint32_t len = N > 0 ? static_cast<uint32_t>(N - 1) : 0;
            std::memcpy(ptr, &len, sizeof(uint32_t));
            if (len > 0) {
                std::memcpy(ptr + sizeof(uint32_t), val, len);
            }
            ptr += sizeof(uint32_t) + len;
        }

        static void serialize_args_all(char*) {
        }

        template <typename... Args>
        static void serialize_args_all(char* ptr, const Args&... args) {
            (..., serialize_arg(ptr, args));
        }

        // ========================================================================
        // 公共的时间格式化缓存函数 (消除冗余代码)
        // ========================================================================
        // 后端解码专用的 TSC 锚点缓存（仅消费线程访问，thread_local 隔离）。
        // 配合 TscClock::tsc_to_unix_ns_cached 省掉每条日志的 seqlock 循环。
        static TscClock::AnchorCache& consumer_anchor_cache() {
            static thread_local TscClock::AnchorCache c;
            return c;
        }

        static inline const char* get_time_format_cache(uint64_t time_ns, uint32_t& out_nano) {
            static thread_local time_t cache_sec = 0;
            static thread_local char cache_str[20] = { 0 };

            time_t sec = static_cast<time_t>(time_ns / 1000000000);
            out_nano = static_cast<uint32_t>(time_ns % 1000000000);

            if (ZRLOG_UNLIKELY(sec != cache_sec)) {
                struct tm tm_buf;
                zrlog_gmtime(&sec, &tm_buf);
                detail::fast_u32_to_4digits(cache_str, tm_buf.tm_year + 1900);
                cache_str[4] = '-';
                detail::fast_u32_to_2digits(cache_str + 5, tm_buf.tm_mon + 1);
                cache_str[7] = '-';
                detail::fast_u32_to_2digits(cache_str + 8, tm_buf.tm_mday);
                cache_str[10] = ' ';
                detail::fast_u32_to_2digits(cache_str + 11, tm_buf.tm_hour);
                cache_str[13] = ':';
                detail::fast_u32_to_2digits(cache_str + 14, tm_buf.tm_min);
                cache_str[16] = ':';
                detail::fast_u32_to_2digits(cache_str + 17, tm_buf.tm_sec);
                cache_sec = sec;
            }
            return cache_str;
        }

        // ========================================================================
        // 静态日志 Decoder: 编译期极端渲染 (AST / 机器码生成)
        // ========================================================================
        template<typename FmtProvider, typename... Args>
        static void generated_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out) {
            uint32_t nano;
            // payload 末尾（不含）：total_size 含 16 字节 header 且已 8 字节对齐，
            // 故 args 区可用字节为 total_size - sizeof(LogEntryHeader)。
            const char* args_end = buffer + ((header.size_tag & kSizeMask) >= sizeof(LogEntryHeader)
                                             ? (header.size_tag & kSizeMask) - sizeof(LogEntryHeader) : 0);
            const char *time_str = get_time_format_cache(
                TscClock::instance().tsc_to_unix_ns_cached(header.tsc, consumer_anchor_cache()), nano);

            // 用大括号初始化列表保证严格的从左到右求值顺序，同时彻底消灭默认构造！
            std::apply([&](const auto&... final_args) {
                // 异常屏障：后端消费线程里任何未捕获异常都会 std::terminate 整个进程。
                // 静态 decoder 用 FMT_COMPILE 生成格式化代码，正常情况下不会抛
                // （格式串是编译期校验过的），但自定义类型的 formatter、内存不足
                // 等仍可能抛，这里兜底成一行 [FMT_ERROR]，绝不让进程挂掉。
                try {
                    // 修复：先判可用量再减 1。若上一条日志恰好填满缓冲（available_size()==0），
                    // 旧的"先减后判"会下溢成 SIZE_MAX，绕过 8192 检查导致 format_to_n 越界写堆
                    if (ZRLOG_UNLIKELY(out.available_size() < 8192)) {
                        out.flush_to_os();
                    }
                    size_t space = out.available_size() - 1;  // 预留 1 字节给 \n

                    auto res = fmt::format_to_n(out.current_ptr(), space, FmtProvider::compile(),
                        fmt::string_view(time_str, 19), nano,
                        loglevel_to_string(meta.level), thread_id,
                        meta.file, meta.line, final_args...);

                    // 处理缓冲区写满换行逻辑
                    if (ZRLOG_LIKELY(res.size <= space)) {
                        out.current_ptr()[res.size] = '\n';
                        out.advance(res.size + 1);
                    }
                    else {
                        out.flush_to_os();
                        space = out.available_size() - 1;

                        res = fmt::format_to_n(out.current_ptr(), space, FmtProvider::compile(),
                            fmt::string_view(time_str, 19), nano,
                            loglevel_to_string(meta.level), thread_id,
                            meta.file, meta.line, final_args...);
                        size_t written = std::min<size_t>(res.size, space);
                        out.current_ptr()[written] = '\n';
                        out.advance(written + 1);
                    }
                }
                catch (const std::exception& e) {
                    write_runtime_error_line(out, meta, time_str, nano, e.what(), false);
                }
                catch (...) {
                    write_runtime_error_line(out, meta, time_str, nano, "unknown exception", false);
                }

            }, std::tuple<typename detail::decode_type<Args>::type...>{ detail::decode_arg<Args>(buffer, args_end)... });
        }

        // ========================================================================
        // 动态日志 Decoder: 头部编译期极速渲染，正文使用链式写入
        // ========================================================================
        template<typename... Args>
        static void generated_runtime_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out) {
            uint32_t nano;
            // payload 末尾（不含），见 generated_decoder 中的说明。
            const char* args_end = buffer + ((header.size_tag & kSizeMask) >= sizeof(LogEntryHeader)
                                             ? (header.size_tag & kSizeMask) - sizeof(LogEntryHeader) : 0);
            const char* time_str = get_time_format_cache(
                TscClock::instance().tsc_to_unix_ns_cached(header.tsc, consumer_anchor_cache()), nano);

            std::apply([&](const auto&... final_args) {
                // 修复：同上，先判可用量再减 1，防止 available_size()==0 时下溢成 SIZE_MAX
                if (ZRLOG_UNLIKELY(out.available_size() < 8192)) {
                    out.flush_to_os();
                }
                size_t space = out.available_size() - 1; // 预留 1 字节给 \n

                try {
                    // 1. 【第一次尝试：极速渲染头部】
                    auto hdr_res = fmt::format_to_n(out.current_ptr(), space,
                        FMT_COMPILE(ZRLOG_HEADER_FMT),
                        fmt::string_view(time_str, 19), nano,
                        loglevel_to_string(meta.level), thread_id,
                        meta.file, meta.line);

                    // 严格区分实际写入大小和理论需要大小
                    size_t actual_hdr_len  = std::min<size_t>(hdr_res.size, space);
                    size_t remaining_space = space - actual_hdr_len;

                    size_t theoretical_total = hdr_res.size;
                    size_t actual_total = actual_hdr_len;

                    // 2. 【第一次尝试：写入正文】
                    if constexpr (sizeof...(Args) == 0) {
                        size_t copy_len = std::min<size_t>(meta.format.size(), remaining_space);
                        std::memcpy(hdr_res.out, meta.format.data(), copy_len);

                        theoretical_total += meta.format.size();
                        actual_total += copy_len;
                    }
                    else {
                        auto body_res = fmt::vformat_to_n(
                            hdr_res.out,
                            remaining_space,
                            fmt::string_view(meta.format.data(), meta.format.size()),
                            fmt::make_format_args(final_args...)
                        );

                        theoretical_total += body_res.size;
                        actual_total += std::min<size_t>(body_res.size, remaining_space);
                    }

                    // 3. 【判断是否需要重试】
                    // 用理论总大小(theoretical_total)与提供的空间比对，而不是用实际写入大小！
                    if (ZRLOG_LIKELY(theoretical_total <= space)) {
                        // 没发生任何截断，安全换行
                        out.current_ptr()[actual_total] = '\n';
                        out.advance(actual_total + 1);
                    }
                    else {
                        // 发生截断！说明当前缓冲区不够长。
                        // 此时之前写入的 actual_total 数据变成了“截断废料”。
                        // 动作：直接落盘刷空，重新来过！
                        out.flush_to_os();
                        space = out.available_size() - 1;

                        // 【重试】：在新缓冲区里重新格式化头部！(非常重要)
                        hdr_res = fmt::format_to_n(out.current_ptr(), space,
                            FMT_COMPILE(ZRLOG_HEADER_FMT),
                            fmt::string_view(time_str, 19), nano,
                            loglevel_to_string(meta.level), thread_id,
                            meta.file, meta.line);

                        actual_hdr_len = std::min<size_t>(hdr_res.size, space);
                        remaining_space = space - actual_hdr_len;
                        actual_total = actual_hdr_len;

                        // 【重试】：在新缓冲区里重新格式化正文！
                        if constexpr (sizeof...(Args) == 0) {
                            size_t copy_len = std::min<size_t>(meta.format.size(), remaining_space);
                            std::memcpy(hdr_res.out, meta.format.data(), copy_len);
                            actual_total += copy_len;
                        }
                        else {
                            auto body_res = fmt::vformat_to_n(
                                hdr_res.out,
                                remaining_space,
                                fmt::string_view(meta.format.data(), meta.format.size()),
                                fmt::make_format_args(final_args...)
                            );
                            actual_total += std::min<size_t>(body_res.size, remaining_space);
                        }

                        // 这一次哪怕再发生截断，也只能认命了(说明日志单条长度超过了整个 IOBuffer 的最大容量)
                        // 强制截断换行，保证内存绝对安全
                        out.current_ptr()[actual_total] = '\n';
                        out.advance(actual_total + 1);
                    }
                }
                catch (const fmt::format_error& e) {
                    // 格式串与参数不匹配（DYN 场景下不同线程并发使用不同格式串）
                    write_runtime_error_line(out, meta, time_str, nano, e.what(), true);
                }
                catch (const std::exception& e) {
                    // 其他异常（bad_alloc、自定义 formatter 抛错等）同样必须被吞掉，
                    // 否则会逃逸到 poll_routine 顶层，导致这条日志被反复重试。
                    write_runtime_error_line(out, meta, time_str, nano, e.what(), false);
                }
                catch (...) {
                    write_runtime_error_line(out, meta, time_str, nano, "unknown exception", false);
                }
            }, std::tuple<typename detail::decode_type<Args>::type...>{ detail::decode_arg<Args>(buffer, args_end)... });
        }

        // 动态 decoder 的错误输出。format_error 时带上原始格式串便于定位；
        // 其他异常只报消息，避免在异常路径上再做一次可能失败的格式化。
        // 本函数自身绝不向外抛异常（A2）。
        //
        // 限流：解码失败会逐条计数，但只输出前 kMaxDecodeErrorLines 行。
        // 若某个 formatter 有 bug 导致大量失败，日志文件不会被错误行淹没；
        // 通过 NanoLogger::decode_error_count() 仍能看到总数。
        static constexpr uint64_t kMaxDecodeErrorLines = 100;

        // 解码失败计数器。decoder 是 static 成员，因此计数用函数内静态量而非
        // 实例成员；消费线程只有一个，但公开访问可能来自其他线程，故用原子量。
        static std::atomic<uint64_t>& decode_error_count_ref() {
            static std::atomic<uint64_t> counter{ 0 };
            return counter;
        }

        // IO 丢弃字节计数：appender 部分写/失败且保留 tail 超过半缓冲时被丢弃的
        // 字节数（如持续 ENOSPC）。与 decode_error_count 同理用函数内静态原子量。
        static std::atomic<uint64_t>& io_drop_bytes_ref() {
            static std::atomic<uint64_t> counter{ 0 };
            return counter;
        }

        static void write_runtime_error_line(IOBuffer& out, const LogMeta& meta,
                                             const char* time_str, uint32_t nano,
                                             const char* what, bool include_format) noexcept {
            const uint64_t seen = decode_error_count_ref().fetch_add(1, std::memory_order_relaxed) + 1;
            if (seen > kMaxDecodeErrorLines) {
                return;  // 已达输出上限，只计数不再写行
            }
            try {
                if (ZRLOG_UNLIKELY(out.available_size() < 512)) {
                    out.flush_to_os();
                }
                size_t err_space = out.available_size() - 1;
                size_t actual_err_len;
                if (include_format) {
                    auto err_msg = fmt::format_to_n(out.current_ptr(), err_space,
                        FMT_COMPILE("[FMT_ERROR] {}.{:09d} {} {}:{} format: '{}' error: '{}'"),
                        fmt::string_view(time_str, 19), nano, loglevel_to_string(meta.level),
                        meta.file, meta.line,
                        fmt::string_view(meta.format.data(), meta.format.size()), what);
                    actual_err_len = std::min<size_t>(err_msg.size, err_space);
                }
                else {
                    auto err_msg = fmt::format_to_n(out.current_ptr(), err_space,
                        FMT_COMPILE("[FMT_ERROR] {}.{:09d} {} {}:{} error: '{}'"),
                        fmt::string_view(time_str, 19), nano, loglevel_to_string(meta.level),
                        meta.file, meta.line, what);
                    actual_err_len = std::min<size_t>(err_msg.size, err_space);
                }
                out.current_ptr()[actual_err_len] = '\n';
                out.advance(actual_err_len + 1);
            }
            catch (...) {
                // 兜底：连错误行都写不出来时，退化为不经 fmt 的纯文本，绝不外抛。
                write_plain_error(out, "[FMT_ERROR] (error line formatting failed)\n");
            }
        }

        // 不经 fmt 的纯文本兜底，异常安全。
        static void write_plain_error(IOBuffer& out, const char* text) noexcept {
            const size_t len = std::strlen(text);
            try {
                if (ZRLOG_UNLIKELY(out.available_size() < len + 1)) {
                    out.flush_to_os();
                }
                if (out.available_size() >= len) {
                    std::memcpy(out.current_ptr(), text, len);
                    out.advance(len);
                }
            }
            catch (...) {
                // 无处可写，静默放弃
            }
        }

        class ThreadBuffer {
        public:
            explicit ThreadBuffer(uint32_t size, uint64_t tid, bool enable_huge_page) : size_(normalize_size(size)), thread_id_(tid) {
                mask_ = size_ - 1;
                mem_block_ = util::allocate_block(size_, enable_huge_page);

                // 【按页预热 Fast Page Pre-faulting】
                // 如果成功吃到了大页，内核已经锁定了连续物理内存，无需预热。
                // 只有在降级到普通 4KB 内存时，才执行原版的强制映射循环。
                // 分配失败（ptr==nullptr，OOM）时跳过，由 valid() 暴露给调用方。
                if (!mem_block_.is_huge_page && mem_block_.ptr) {
                    volatile char *p = mem_block_.ptr;
                    for (size_t i = 0; i < size_; i += 4096) {
                        p[i] = 0;
                    }
                }
            }

            ~ThreadBuffer() {
                util::free_block(mem_block_);
            }

            inline uint64_t thread_id() const {
                return thread_id_;
            }

            // 内存块是否分配成功。OOM 时 allocate_block 返回空指针，调用方据此
            // 放弃入队（按丢弃处理），避免对空指针预热/写入导致崩溃。
            inline bool valid() const {
                return mem_block_.ptr != nullptr;
            }

            // ------------------------------------------------------------------------
            // 前端（生产者）接口
            // ------------------------------------------------------------------------
            // 快路径（热）：空间检查 + 物理位置计算 + 尾部空间判断 + 返回指针，
            // 约 10 条指令。绕回代码（padding + 游标推进 + 发布）拆到 noinline
            // alloc_wrap()：8MB 缓冲下每 8MB 才触发一次，独立函数边界让快路径
            // 保持极小体积，MSVC 内联进 push_log_entry 时不会撑大寄存器压力
            // （此前 alloc forceinline 退化正是因为把绕回代码一起内联了）。
            char* alloc(uint32_t len) {
                // 生产者是 write_index_ 的唯一写者，本地游标即最新值，无需 load。
                uint64_t w = local_write_index_;

                // 【消除 P99 抖动】
                // 使用非原子的本地缓存游标 `cached_read_index_` 判断空间。
                // 在 99% 的情况下，直接 0 成本过检，彻底避免跨核心总线通信！
                if (ZRLOG_UNLIKELY(w + len - cached_read_index_ > size_)) {
                    // 本地认为空间不够时，才付出代价去同步消费者最新的实际进度
                    cached_read_index_ = read_index_.load(std::memory_order_acquire);
                    if (ZRLOG_UNLIKELY(w + len - cached_read_index_ > size_)) {
                        return nullptr;
                    }
                }

                uint32_t phys_w = w & mask_;
                uint32_t tail_free = size_ - phys_w;

                // 物理尾部连续空间足够，直接分配（绝大多数日志走这条路）
                if (ZRLOG_LIKELY(len <= tail_free)) {
                    return mem_block_.ptr + phys_w;
                }

                // 物理尾部空间不够，必须绕回——冷路径
                return alloc_wrap(len);
            }

            // 绕回（冷路径）：写 padding 占位、推进游标到下一圈、提前发布 padding。
            // 从 local_write_index_ 重新取值（单生产者，安全），避免把 w/phys_w/
            // tail_free 作为参数传入——那是快路径的寄存器，传参反而增加压力。
            ZRLOG_NOINLINE char* alloc_wrap(uint32_t len) {
                uint64_t w = local_write_index_;
                uint32_t phys_w = w & mask_;
                uint32_t tail_free = size_ - phys_w;

                // 再次检查逻辑空间 (加上绕回产生的 padding 浪费后) 是否够用
                if (ZRLOG_UNLIKELY(w + tail_free + len - cached_read_index_ > size_)) {
                    cached_read_index_ = read_index_.load(std::memory_order_acquire);
                    if (ZRLOG_UNLIKELY(w + tail_free + len - cached_read_index_ > size_)) {
                        return nullptr;
                    }
                }

                // 写入 Padding 占位符
                if (ZRLOG_LIKELY(tail_free >= sizeof(LogEntryHeader))) {
                    write_padding_local(phys_w, tail_free);
                }

                // 推进逻辑写下标，补齐到下一圈的物理 0 处
                w += tail_free;
                local_write_index_ = w;

                // 提前发布 Padding，让消费者可以看到绕回动作。这是一次独立于
                // commit() 的发布：消费者必须先跳过尾部 padding，才能看到随后
                // commit() 发布的新一圈条目，两次 store 的顺序不能合并/颠倒。
                write_index_.store(w, std::memory_order_release);

                return mem_block_.ptr;
            }

            inline void commit(uint32_t len) {
                // 前端热路径的唯一一次跨核写：推进本地游标并一次性发布。
                // alloc() 与 commit() 不再各自 load(relaxed) write_index_；
                // 既能少一条 load，也让编译器不必保守地假设中间有外部写者。
                local_write_index_ += len;
                write_index_.store(local_write_index_, std::memory_order_release);
            }

            // ------------------------------------------------------------------------
            // 后台（消费者）接口
            // ------------------------------------------------------------------------
            LogEntryHeader* try_read() {
                //【消费者全本地化】
                // 消费者直接使用本地游标 local_read_index_，完全避开 atomic load
                uint64_t r = local_read_index_;

                if (ZRLOG_UNLIKELY(r >= cached_write_index_)) {
                    cached_write_index_ = write_index_.load(std::memory_order_acquire);
                    if (ZRLOG_UNLIKELY(r >= cached_write_index_)) {
                        return nullptr;
                    }
                }

                constexpr int MAX_SKIPS = 8;
                int skips = 0;

                while (ZRLOG_LIKELY(skips < MAX_SKIPS && r < cached_write_index_)) {
                    uint32_t phys_r = r & mask_;
                    uint32_t tail_avail = size_ - phys_r;

                    // 1. 隐式 Padding：尾部连 Header 都读不全
                    if (ZRLOG_UNLIKELY(tail_avail < sizeof(LogEntryHeader))) {
                        r += tail_avail;
                        local_read_index_ = r;
                        // 绕回时为了防止生产者卡死，强制推送一次游标
                        read_index_.store(r, std::memory_order_release);
                        continue;
                    }

                    LogEntryHeader *header = reinterpret_cast<LogEntryHeader *>(mem_block_.ptr + phys_r);

                    // 2. 显式 Padding
                    if (ZRLOG_UNLIKELY((header->size_tag >> kTagShift) == kTagPadding)) {
                        uint32_t claimed = (header->size_tag & kSizeMask);
                        if (ZRLOG_UNLIKELY(claimed < sizeof(LogEntryHeader) || claimed > tail_avail)) {
                            return nullptr;
                        }
                        r += claimed;
                        local_read_index_ = r;
                        // 遇到大块 padding 强制推送，迅速释放空间
                        read_index_.store(r, std::memory_order_release);
                        ++skips;
                        continue;
                    }

                    // 3. 拦截未完全 Commit 的块
                    if (ZRLOG_UNLIKELY(r + (header->size_tag & kSizeMask) > cached_write_index_)) {
                        cached_write_index_ = write_index_.load(std::memory_order_acquire);
                        if (ZRLOG_UNLIKELY(r + (header->size_tag & kSizeMask) > cached_write_index_)) {
                            return nullptr;
                        }
                    }

                    return header;
                }

                return nullptr;
            }

            inline void consume(uint32_t len) {
                local_read_index_ += len;

                // 【游标批量提交 (Batch Commit)】
                // 避免消费者疯狂 store 导致生产者 L1 Cache 失效。
                // 每积攒 4096 字节（4KB）才提交一次给生产者看。
                // 利用极速位运算判断是否跨越了 4096 边界 (0xFFF = 4095)。
                if (ZRLOG_LIKELY((local_read_index_ & 0xFFF) < len)) {
                    read_index_.store(local_read_index_, std::memory_order_release);
                }
            }

            // 在消费者准备休眠、或者退出前必须调用，确保存留的进度被完全推送给生产者
            inline void flush_consume() {
                if (read_index_.load(std::memory_order_relaxed) != local_read_index_) {
                    read_index_.store(local_read_index_, std::memory_order_release);
                }
            }

            // ------------------------------------------------------------------------
            // 杂项与监控接口
            // ------------------------------------------------------------------------
            inline bool should_deallocate() const {
                // 修复：跨线程读写（垂死线程写、后台线程轮询读），改为原子消除 data race；
                // 冷路径元数据，原子化零性能成本
                return should_deallocate_.load(std::memory_order_acquire);
            }
            inline void mark_deallocate() {
                should_deallocate_.store(true, std::memory_order_release);
            }
            inline uint32_t capacity() const {
                return size_;
            }

            inline uint32_t estimate_used_space() const {
                uint64_t w = write_index_.load(std::memory_order_acquire);
                uint64_t r = read_index_.load(std::memory_order_acquire);
                return (w > r) ? static_cast<uint32_t>(w - r) : 0;
            }

            inline uint32_t estimate_free_space() const {
                return size_ - estimate_used_space();
            }

        private:
            // 归一化为 2 的幂，最小 1024，最大 2^30
            inline constexpr uint32_t normalize_size(uint32_t n) noexcept {
                constexpr uint32_t MIN_SIZE = 1024;
                constexpr uint32_t MAX_SIZE = 1u << 30;

                if (n <= MIN_SIZE) {
                    return MIN_SIZE;
                }

                if (n >= MAX_SIZE) {
                    return MAX_SIZE;
                }

                --n;

                n |= n >> 1;
                n |= n >> 2;
                n |= n >> 4;
                n |= n >> 8;
                n |= n >> 16;

                ++n;
                return n;
            }

            inline void write_padding_local(uint32_t pos, uint32_t pad_size) {
                LogEntryHeader *p = reinterpret_cast<LogEntryHeader *>(mem_block_.ptr + pos);
                p->size_tag = (kTagPadding << kTagShift) | (pad_size & kSizeMask);
                p->meta_id = 0;
                p->tsc = 0;
            }

            // =========================================================================
            // 【跨核缓存行物理隔离】
            //
            // 隔离粒度用 128 字节而非 64 字节（ZRLOG_CROSS_THREAD_STRIDE）：
            // Intel 的 L2 adjacent-cache-line prefetcher 一次会拉取 128 字节对齐
            // 的【一对】缓存行。若两个被不同核心高频写的字段只隔 64 字节，预取器
            // 会把对方那条行也带进自己的 L2，跨核所有权转移（RFO）照旧发生——
            // 64 字节隔离在该架构上并不够。实测缓冲满时生产者每条日志都要读
            // read_index_（见 alloc），正是这条路径上的争抢。
            // =========================================================================

            // --- 生产者独占 (写入端) ---
            // write_index_ 是唯一的跨核发布点：只有 commit()（以及绕回时提前
            // 发布 padding）才 store(release)，消费者据此判断条目可见。
            alignas(ZRLOG_CROSS_THREAD_STRIDE) std::atomic<uint64_t> write_index_{ 0 };
            // local_write_index_ / cached_read_index_ 都是生产者私有：消费者从不
            // 触碰。前端在 alloc()/commit() 之间只读写 local_write_index_，不再
            // 对 write_index_ 做两次 relaxed load；commit() 里把它一次性 store
            // 到 write_index_ 完成发布。二者可共享生产者私有行，无需单独隔离。
            uint64_t local_write_index_{ 0 };
            uint64_t cached_read_index_{ 0 };

            // --- 消费者独占 (读取端) ---
            // 与 write_index_ 至少隔 128 字节：两者分属不同核心的写者，是全库
            // 唯一真正跨核共享的游标对。
            alignas(ZRLOG_CROSS_THREAD_STRIDE) std::atomic<uint64_t> read_index_{ 0 };
            uint64_t cached_write_index_{ 0 };
            uint64_t local_read_index_{ 0 };

            // --- 只读元数据 ---
            // size_/mask_ 每条日志都读（热），但只读不写，不会引发所有权转移，
            // 与冷字段同处一行没有实际代价。
            alignas(ZRLOG_CROSS_THREAD_STRIDE) uint32_t size_;
            uint32_t mask_;
            uint64_t thread_id_;

            // ========================================================================
            // 替换 原来的std::vector，使用原生指针与内存块元数据
            // ========================================================================
            util::MemoryBlock mem_block_;

            // 线程退出时由垂死线程写、后台线程轮询读；冷路径，单独一行避免
            // 与热字段共享导致任何形式的干扰。
            alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<bool> should_deallocate_{ false };
        };

        class ThreadBufferDestroyer {
        public:
            explicit ThreadBufferDestroyer() {
            }

            ~ThreadBufferDestroyer() {
                if (nullptr != NanoLogger::thread_buffer_) {
                    // 标记缓冲区待释放，由后台线程排空后 delete。
                    NanoLogger::thread_buffer_->mark_deallocate();
                    if (logger_) {
                        logger_->notify_consumer();
                    }
                    // 置空 TLS 指针：缓冲已交给消费者排空后 delete，若本线程其它
                    // TLS 析构函数再日志，get_thread_buffer 见空指针会另建缓冲（或
                    // 在 shutdown 后丢弃），绝不复用这块即将被释放的内存（防 UAF）。
                    NanoLogger::thread_buffer_ = nullptr;
                }
            }

            inline void init(NanoLogger* logger) {
                logger_ = logger;
            }
            
        private:
            NanoLogger *logger_{ nullptr };
        };

        template<typename FmtProvider, typename... Args>
        uint32_t register_log_meta(std::atomic<uint32_t>& log_id_atom, LogLevel level, 
            std::string_view file, uint32_t line, std::string_view func) {
            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
            uint32_t id = log_id_atom.load(std::memory_order_relaxed);
            if (id != 0) {
                return id;
            }

            uint32_t new_id = static_cast<uint32_t>(global_log_metas_.size());
            LogMeta new_log;
            new_log.id = new_id;
            new_log.level = level;
            new_log.file = file;
            new_log.line = line;
            new_log.func = func;
            new_log.decoder = &generated_decoder<FmtProvider, Args...>;
            global_log_metas_.push_back(std::move(new_log));
            meta_count_hint_.store(global_log_metas_.size(), std::memory_order_release);

            log_id_atom.store(new_id, std::memory_order_relaxed);
            return new_id;
        }

        template<typename... Args>
        uint32_t register_runtime_log_meta(LogLevel level,
            std::string_view file, uint32_t line, std::string_view func, std::string_view format) {
            std::lock_guard<SpinMutex> lock(log_metas_mutex_);

            // 按 (file, line, format 内容) 查重：同一调用点在同一格式串上反复
            // 切换（A->B->A）时复用已有 meta，避免元数据表无界增长。
            // 用内容哈希索引把查重从 O(N) 全表扫描降到 O(1) 摊还——旧实现在自旋锁
            // 内遍历全部 meta，调用点很多时锁持有时间线性增长，抬高前端注册尖峰。
            // 哈希碰撞由下面的全内容比对兜底；必须比对内容而非指针：std::string
            // 原地修改后 data() 不变。
            const uint64_t h = detail::fnv1a_hash(format);
            auto it = runtime_meta_index_.find(h);
            if (it != runtime_meta_index_.end()) {
                for (uint32_t id : it->second) {
                    const LogMeta& m = global_log_metas_[id];
                    // decoder 比对限定为同参数类型的 runtime meta，避免误匹配静态 meta
                    if (m.decoder == &generated_runtime_decoder<Args...> &&
                        m.line == line && m.file == file && m.format.size() == format.size() &&
                        (format.empty() || std::memcmp(m.format.data(), format.data(), format.size()) == 0)) {
                        return id;
                    }
                }
            }

            uint32_t new_id = static_cast<uint32_t>(global_log_metas_.size());
            LogMeta new_log;
            new_log.id = new_id;
            new_log.level = level;
            new_log.file = file;
            new_log.line = line;
            new_log.func = func;
            // 运行时注册深拷贝 format，用于后续的 fmt::runtime
            new_log.format = std::string(format);
            new_log.decoder = &generated_runtime_decoder<Args...>;
            global_log_metas_.push_back(std::move(new_log));
            runtime_meta_index_[h].push_back(new_id);
            meta_count_hint_.store(global_log_metas_.size(), std::memory_order_release);

            return new_id;
        }

        ZRLOG_FORCEINLINE ThreadBuffer* get_thread_buffer() {
            // 热路径只读取 TLS 指针。把首次创建、OOM、注册等冷逻辑独立出去，
            // 避免 MSVC 为它们保存大量寄存器并建立栈帧，拖累每条日志。
            ThreadBuffer* tb = thread_buffer_;
            if (ZRLOG_LIKELY(tb != nullptr)) {
                return tb;
            }
            return create_thread_buffer();
        }

        ZRLOG_NOINLINE ThreadBuffer* create_thread_buffer() {
            // 已 shutdown（fini / 静态析构 / 崩溃抢救结束）：不再创建缓冲，日志
            // 按丢弃处理。既避免向无人消费的缓冲入队造成泄漏，也避免在单例析构
            // 后触碰已销毁的 config_（std::string）。冷路径判断，无热路径开销。
            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                return nullptr;
            }
            ThreadBuffer* tb = new ThreadBuffer(config_.thread_buffer_size, util::get_thread_id(), config_.enable_huge_page);
            if (ZRLOG_UNLIKELY(!tb->valid())) {
                // 内存分配失败（OOM）：不注册、不入队，前端按丢弃处理。
                delete tb;
                return nullptr;
            }
            thread_buffer_ = tb;
            thread_buffer_destroyer_.init(this);
            std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
            thread_buffers_.push_back(thread_buffer_);
            buffer_count_hint_.store(buffer_count_hint_.load(std::memory_order_relaxed) + 1,
                                     std::memory_order_release);
            return tb;
        }

        size_t consume_buffers_round_robin(std::vector<LogMeta>& local_log_metas, IOBuffer& io_buf, bool full_drain = false) {
            size_t processed_count = 0;
            // 崩溃抢救路径：垂死线程可能正持有某把锁，此时强自旋会永久卡死，
            // 使抢救失败。故崩溃模式下改用 try_lock，抢不到就跳过本次同步，
            // 用已缓存的元数据/缓冲尽力输出剩余日志。
            const bool crashed = is_crashed_.load(std::memory_order_acquire);

            {
                // 快速路径：注册计数没变化就不必抢锁。这个判断是无锁的，
                // 高频轮询（每轮都会走到这里）时可省掉一次自旋锁的
                // test_and_set + 缓存行失效。
                const bool maybe_new_meta =
                    meta_count_hint_.load(std::memory_order_acquire) > local_log_metas.size();
                if (maybe_new_meta) {
                    if (crashed) {
                        if (log_metas_mutex_.try_lock()) {
                            if (global_log_metas_.size() > local_log_metas.size()) {
                                local_log_metas.insert(local_log_metas.end(),
                                    global_log_metas_.begin() + local_log_metas.size(),
                                    global_log_metas_.end());
                            }
                            log_metas_mutex_.unlock();
                        }
                    }
                    else {
                        std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                        if (global_log_metas_.size() > local_log_metas.size()) {
                            local_log_metas.insert(local_log_metas.end(),
                                global_log_metas_.begin() + local_log_metas.size(),
                                global_log_metas_.end());
                        }
                    }
                }
            }
            {
                // 【连续内存预取】将新增的 Buffer 高效转移到后台集合
                // 注意：新加入的 Buffer 都在 vector 尾部，天生属于“非活跃区”，
                // 下一次纪元扫描时如果有数据，自然会被提拔到前面。
                // 与"累计并入数"比较，而非 bg_.size()：线程退出会从 bg_ 删除条目，
                // 用 size() 会让该不等式永久成立、快速路径失效。
                const bool maybe_new_buffer =
                    buffer_count_hint_.load(std::memory_order_acquire) > buffers_merged_total_;
                if (maybe_new_buffer) {
                    if (crashed) {
                        if (thread_buffers_mutex_.try_lock()) {
                            if (!thread_buffers_.empty()) {
                                buffers_merged_total_ += thread_buffers_.size();
                                thread_buffers_bg_.insert(thread_buffers_bg_.end(), thread_buffers_.begin(), thread_buffers_.end());
                                thread_buffers_.clear();
                            }
                            thread_buffers_mutex_.unlock();
                        }
                    }
                    else {
                        std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                        if (!thread_buffers_.empty()) {
                            buffers_merged_total_ += thread_buffers_.size();
                            thread_buffers_bg_.insert(thread_buffers_bg_.end(), thread_buffers_.begin(), thread_buffers_.end());
                            thread_buffers_.clear();
                        }
                    }
                }
            }

            if (thread_buffers_bg_.empty()) {
                return 0;
            }

            ++poll_cycles_;

            // =========================================================================
            // 1. 决定扫描策略 (Epoch Scan)
            // =========================================================================
            // 如果是全量 Drain，或者每 64 轮，或者当前没活跃线程，进行全量扫描
            bool is_full_scan = full_drain || (poll_cycles_ % 64 == 0) || (active_buffer_count_ == 0);
            size_t scan_limit = is_full_scan ? thread_buffers_bg_.size() : active_buffer_count_;

            LogEntryHeader* header;

            // =========================================================================
            // 2. O(1) 原地分区遍历与消费算法
            // =========================================================================
            for (size_t i = 0; i < scan_limit; /* 注意：这里无 i++ */) {
                ThreadBuffer *tb = thread_buffers_bg_[i];
                uint32_t quota = full_drain ? UINT32_MAX : config_.per_thread_quota;
                bool current_buffer_has_data = false;

                // --- 业务消费逻辑开始 ---
                while ((quota > 0) && (header = tb->try_read())) {
                    current_buffer_has_data = true; // 标记本轮读到了数据
                    ++processed_count;
                    --quota;

                    // 先解码、后推进游标。解码参数是指向环形缓冲内部的零拷贝
                    // string_view，header 也按引用读取：若先 consume 发布 read_index_，
                    // 生产者可能在 decoder 阻塞落盘期间绕回一整圈，覆盖正在解码的
                    // 条目，造成日志内容撕裂/时间戳跳变（背压 + 慢盘场景）。
                    // 毒丸防护改由"调用点全量 catch + 无条件 consume"保证：即便
                    // decoder 意外抛出，游标也照样前进，绝不让坏日志被反复重试。
                    const uint32_t entry_size = header->size_tag & kSizeMask;

                    const LogMeta* meta = nullptr;
                    const uint32_t meta_id = header->meta_id;
                    if (meta_id > 0 && meta_id < local_log_metas.size()) {
                        meta = &local_log_metas[meta_id];
                    }
                    else {
                        // log_id 尚未同步到本地：再拉取一次元数据。崩溃模式下用
                        // try_lock——垂死线程可能正持有该锁，强自旋会永久卡死抢救。
                        bool locked;
                        if (crashed) {
                            locked = log_metas_mutex_.try_lock();
                        }
                        else {
                            log_metas_mutex_.lock();
                            locked = true;
                        }
                        if (locked) {
                            if (global_log_metas_.size() > local_log_metas.size()) {
                                local_log_metas.insert(local_log_metas.end(),
                                    global_log_metas_.begin() + local_log_metas.size(),
                                    global_log_metas_.end());
                            }
                            log_metas_mutex_.unlock();
                        }
                        if (meta_id > 0 && meta_id < local_log_metas.size()) {
                            meta = &local_log_metas[meta_id];
                        }
                    }

                    if (meta) {
                        char* args_ptr = (char*)header + sizeof(LogEntryHeader);
                        try {
                            meta->decoder(args_ptr, *meta, *header, tb->thread_id(), io_buf);
                        }
                        catch (...) {
                            // decoder 内部本应全量 catch；此处兜底确保异常不会
                            // 阻止下面的 consume（毒丸防护的最后一道防线）。
                        }
                    }
                    // 无论 meta 是否存在、decoder 是否抛出，游标都无条件前进，
                    // 坏日志绝不重试（毒丸防护）。
                    tb->consume(entry_size);
                }

                // 一轮消费完了，如果是最后的数据，强制把游标更新出去
                tb->flush_consume();
                // --- 业务消费逻辑结束 ---

                // =========================================================================
                // 3. 处理线程死亡回收 (Deallocate)
                // =========================================================================
                if (tb->should_deallocate() && tb->estimate_used_space() == 0) {
                    delete tb;

                    // 【极其关键的多重 swap 逻辑】：保护活跃分区边界不被破坏
                    if (i < active_buffer_count_) {
                        // 1. 如果死亡的线程在“活跃区”，先把它和活跃区的最后一个元素交换
                        --active_buffer_count_;
                        if (i != active_buffer_count_) {
                            std::swap(thread_buffers_bg_[i], thread_buffers_bg_[active_buffer_count_]);
                        }
                        // 2. 现在死亡元素在 active_buffer_count_ 的位置（属于非活跃区开头），
                        // 再把它和整个 vector 的最后一个元素交换，然后 pop
                        if (active_buffer_count_ != thread_buffers_bg_.size() - 1) {
                            std::swap(thread_buffers_bg_[active_buffer_count_], thread_buffers_bg_.back());
                        }
                    }
                    else {
                        // 如果死亡的线程本来就在“非活跃区”，直接和 vector 最后一个元素交换并 pop
                        if (i != thread_buffers_bg_.size() - 1) {
                            std::swap(thread_buffers_bg_[i], thread_buffers_bg_.back());
                        }
                    }

                    thread_buffers_bg_.pop_back();
                    --scan_limit; // 删除了一个元素，扫描上限必须 -1

                    // 元素已删除，当前索引 i 填入了新的未知元素，直接 continue 进入下一轮检查，绝对不能 ++i
                    continue;
                }

                // =========================================================================
                // 4. 活跃区提拔 (Promotion) 与 降级 (Demotion)
                // =========================================================================
                if (current_buffer_has_data) {
                    // 【提拔】：发现数据，如果在非活跃区，提拔进活跃区
                    if (i >= active_buffer_count_) {
                        std::swap(thread_buffers_bg_[i], thread_buffers_bg_[active_buffer_count_]);
                        ++active_buffer_count_;
                    }
                    ++i; // 当前槽位处理完毕，前进
                }
                else {
                    // 【降级】：没有数据，如果霸占着活跃区，踢出去
                    if (i < active_buffer_count_) {
                        --active_buffer_count_;
                        std::swap(thread_buffers_bg_[i], thread_buffers_bg_[active_buffer_count_]);
                        // 注意这里不 ++i，因为换过来的新元素在位置 i，需要下一轮检查
                    }
                    else {
                        // 本就在非活跃区且无数据，正常前进
                        ++i;
                    }
                }
            } // end for

            return processed_count;
        }

        void poll_routine() {

            // ========================================================================
            // 执行后台线程绑核 (CPU Physical Isolation)
            // ========================================================================
            // 优先级：显式 core_ids > 自动绑 P-core（opt-in）> OS 默认调度
            if (!config_.background_thread_core_ids.empty()) {
                if (!util::pin_thread_to_cores(config_.background_thread_core_ids)) {
                    const char err_msg[] = "[ZRLOG WARNING] Failed to pin background thread to specified CPU cores.\n";
                    [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg)-1);
                }
            }
            else if (config_.auto_pin_to_p_cores) {
                // 仅在异构架构上绑核；同构架构绑了也没意义，反而可能与业务线程争抢
                if (util::has_hybrid_cores()) {
                    std::vector<int> p_cores = util::get_performance_core_ids();
                    if (!p_cores.empty() && !util::pin_thread_to_cores(p_cores)) {
                        const char err_msg[] = "[ZRLOG WARNING] Failed to auto-pin background thread to P-cores.\n";
                        [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg)-1);
                    }
                }
            }

            std::vector<LogMeta> local_log_metas;
            local_log_metas.reserve(1000);
            IOBuffer io_buf(appender_.get(), config_.io_buffer_size);

            auto last_force_flush_time = std::chrono::steady_clock::now();
            auto last_tsc_sync_time = last_force_flush_time;

            // 自适应空闲等待从最短间隔起步
            idle_wait_us_ = std::max<uint32_t>(1, config_.idle_busy_interval_us);

            while (log_thread_running_.load(std::memory_order_relaxed)) {

                // 顶层异常屏障：filesystem / vector 扩容 / 自定义 formatter 等
                // 抛出的异常绝不能逃出后台线程（std::thread 里逃逸 = std::terminate）。
                // 单轮失败只跳过本轮，下轮继续，保证日志系统自身不拖垮进程。
                try {

                    // 【崩溃检测】发现发生严重崩溃，立即启动抢救式全量 Drain
                    if (ZRLOG_UNLIKELY(is_crashed_.load(std::memory_order_acquire))) {
                        // 有界抢救：其他业务线程可能仍在产日志，无界 drain 在高负载下
                        // 永远追不上。用 rdtsc 预算（与 emergency_flush 同量级）兜底，
                        // 保证有限时间内退出，而非依赖垂死线程的超时。
                        const double npt = crash_ns_per_tick_ > 0 ? crash_ns_per_tick_ : 1.0;
                        const uint64_t budget_ticks = static_cast<uint64_t>(
                            static_cast<double>(config_.crash_drain_wait_ms) * 1e6 / npt);
                        const uint64_t deadline = TscClock::rdtsc() + budget_ticks;
                        while (consume_buffers_round_robin(local_log_metas, io_buf, true) > 0) {
                            if (TscClock::rdtsc() > deadline) {
                                break;
                            }
                        }
                        io_buf.flush_force();
                        crash_drain_done_.store(true, std::memory_order_release);
                        // 置停止标志：让 Block/Retry 策略的生产者尽快退出自旋，
                        // 避免对着已退出的消费者永久空转。
                        log_thread_running_.store(false, std::memory_order_relaxed);
                        return; // 抢救完毕，安全退出
                    }

                    size_t process_count = consume_buffers_round_robin(local_log_metas, io_buf, false);
                    auto now = std::chrono::steady_clock::now();

                    // ========================================================================
                    // 周期性物理落盘(防断电/内核崩溃)
                    // ========================================================================
                    // 无论系统是处于空闲还是极端高负载，间隔force_flush_interval时长必然触发一次真实的 fsync
                    // 这保证了在面临机柜断电等最高级别物理灾难时，最多只丢失 force_flush_interval 毫秒的日志
                    // force_flush_interval_ms == 0 表示关闭周期 fsync（见 Config 注释）。
                    if (ZRLOG_UNLIKELY(config_.force_flush_interval_ms != 0)) {
                        auto force_flush_interval = std::chrono::milliseconds(config_.force_flush_interval_ms);
                        if (ZRLOG_UNLIKELY(now - last_force_flush_time >= force_flush_interval)) {
                            io_buf.flush_force();
                            last_force_flush_time = now;
                        }
                    }
                    // ========================================================================
                    // 周期性 TSC 时钟漂移校准 (以tsc_sync_interval为间隔)
                    // ========================================================================
                    auto tsc_sync_interval = std::chrono::seconds(config_.tsc_sync_interval_s);
                    if (ZRLOG_UNLIKELY(now - last_tsc_sync_time >= tsc_sync_interval)) {
                        // 使用已有的 sync_system_time，它利用 seq_ 实现了 Seqlock，
                        // 保证前端线程读取时间时绝不会读到撕裂的 base_ns 和 base_tsc
                        TscClock::instance().sync_system_time();
                        last_tsc_sync_time = now;
                    }

                    if (process_count < 1) {  // idle
                        // 空闲时，立刻将当前用户态缓冲交由操作系统 (Page Cache)
                        // 保证了低延迟情况下的 tail -f 实时可见性
                        io_buf.flush_to_os();

                        // 自适应空闲等待：刚处理完数据（或有积压）时用较短的间隔，
                        // 让后续日志尽快落盘；连续多轮无数据则指数退避到上限，省 CPU。
                        // 注意仍保留"持锁置位 + 持锁 notify"协议（修复竞态 1），
                        // 自适应只改变等待时长，不改变同步语义。
                        uint32_t wait_us;
                        if (config_.adaptive_idle_wait) {
                            const uint32_t busy_us = std::max<uint32_t>(1, config_.idle_busy_interval_us);
                            const uint32_t max_us = std::max<uint32_t>(busy_us, config_.idle_wait_interval_us);
                            wait_us = idle_wait_us_;
                            // 下一轮若仍空闲则放大（上限 max_us）
                            idle_wait_us_ = std::min<uint32_t>(max_us, idle_wait_us_ * 2u);
                        }
                        else {
                            wait_us = config_.idle_wait_interval_us;
                        }

                        {
                            std::unique_lock<std::mutex> lock(idle_wait_mutex_);
                            // 置位必须在锁内：与持锁 notify 配对，保证生产者看到 flag==true 时
                            // 消费者必然已在 wait_for 中，通知不会丢（修复竞态 1）
                            idle_wait_flag_.store(true, std::memory_order_relaxed);
                            idle_wait_condition_.wait_for(lock, std::chrono::microseconds(wait_us));
                        }
                        idle_wait_flag_.store(false, std::memory_order_relaxed);
                    }
                    else {
                        // 处理到数据：回到最短等待间隔，保证突发流量后的 tail 及时可见
                        if (config_.adaptive_idle_wait) {
                            idle_wait_us_ = std::max<uint32_t>(1, config_.idle_busy_interval_us);
                        }
                    }

                }  // end try
                catch (const std::exception& e) {
                    const char msg[] = "[ZRLOG] background consumer exception: ";
                    [[maybe_unused]] auto r1 = ZRLOG_WRITE(STDERR_FILENO, msg, sizeof(msg) - 1);
                    [[maybe_unused]] auto r2 = ZRLOG_WRITE(STDERR_FILENO, e.what(), std::strlen(e.what()));
                    [[maybe_unused]] auto r3 = ZRLOG_WRITE(STDERR_FILENO, "\n", 1);
                }
                catch (...) {
                    const char msg[] = "[ZRLOG] background consumer unknown exception\n";
                    [[maybe_unused]] auto r = ZRLOG_WRITE(STDERR_FILENO, msg, sizeof(msg) - 1);
                }
            }

            // ==================== shutdown 全量 drain（绕过 256 限制） ====================
            // 注意：full_drain = true 时，consume 内部的 scan_limit 会强制为 size()，保证彻底清空
            while (consume_buffers_round_robin(local_log_metas, io_buf, true) > 0) {
            }

            io_buf.flush_force();
        }

        inline void notify_consumer() {
            // 忙时热路径：一次 relaxed load + 分支，零成本不变。
            // 消费者置位 flag 在锁内完成（见 poll_routine），故看到 flag==true 时
            // 消费者必然已进入 wait_for，持锁 notify 通知必达（修复竞态 1）。
            // 竞态 2（生产者检查 flag 时消费者尚未置位的宽窗口，命中率 ~2%）由
            // idle_wait_interval_us 超时兜底，属可接受的设计取舍。
            // 节流扩大了竞态 2 的窗口（生产者可能跳过检查），但超时兜底
            // (idle_wait_interval_us, 默认 1ms) >> 节流阈值 (50µs)，安全。
            if (idle_wait_flag_.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(idle_wait_mutex_);
                idle_wait_flag_.store(false, std::memory_order_relaxed);
                idle_wait_condition_.notify_one();
            }
        }

        NanoLogger() {
            TscClock::instance();
            thread_buffers_.reserve(100);
            thread_buffers_bg_.reserve(100);
            //新增log_id为0的LogMeta项,保证有效log_id的序号从1开始
            global_log_metas_.emplace_back(LogMeta());
        }
        ~NanoLogger() {
            fini();
        }

        static ZRLOG_FAST_THREAD_LOCAL ThreadBuffer* thread_buffer_;
        static thread_local ThreadBufferDestroyer thread_buffer_destroyer_;

        Config config_;
        // 热状态打包：一个 32 位原子量同时携带"生效日志级别"与"逐条通知开关"，
        // 热路径一次 relaxed load 即可完成级别判断和通知判断，消除原先
        // active_level_ 与 config_.notify_consumer_per_log 两次跨缓存行读。
        // bits 0-2 = LogLevel (0-5)，bit 3 = notify_consumer_per_log。
        static constexpr uint32_t kHotLevelMask = 0x7u;
        static constexpr uint32_t kHotNotifyBit = 0x8u;
        alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<uint32_t> hot_state_{
            static_cast<uint32_t>(LogLevel::INFO) | kHotNotifyBit };
        SpinMutex log_metas_mutex_;
        std::deque<LogMeta> global_log_metas_;
        // DYN 格式串内容哈希 -> 候选 meta id 列表，加速 register_runtime_log_meta
        // 查重（O(1) 摊还）。仅在 log_metas_mutex_ 锁内访问；碰撞由全内容比对兜底。
        std::unordered_map<uint64_t, std::vector<uint32_t>> runtime_meta_index_;
        SpinMutex thread_buffers_mutex_;
        std::vector<ThreadBuffer*> thread_buffers_;
        std::vector<ThreadBuffer*> thread_buffers_bg_;

        size_t active_buffer_count_{ 0 };
        uint64_t poll_cycles_{ 0 };
        // 无锁"是否有新注册"提示：注册侧 store 总条数，消费侧先读它，
        // 只有确实变多了才去抢自旋锁。省掉高频轮询时的无谓加锁。
        std::atomic<size_t> meta_count_hint_{ 1 };    // 1 = 占位 meta(id=0)
        std::atomic<size_t> buffer_count_hint_{ 0 };
        // 累计已并入后台集合的 buffer 数（仅消费线程写）。与 buffer_count_hint_
        // 比较判断是否有新 buffer，不受线程退出删除条目影响。
        size_t buffers_merged_total_{ 0 };
        // 自适应空闲等待的当前间隔（仅后台线程读写，无需原子）
        uint32_t idle_wait_us_{ 50 };

        std::unique_ptr<ILogAppender> appender_;
        std::thread log_thread_;
        std::atomic<bool> log_thread_running_{ false };

        std::mutex              idle_wait_mutex_;
        std::condition_variable idle_wait_condition_;
        alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<bool> idle_wait_flag_{ false };

    public:
        // 已注册的日志元数据条数（含 id=0 的占位项）。用于观测 DYN 格式串
        // 反复切换时元数据表是否收敛；只读、无副作用。
        size_t log_meta_count() const {
            std::lock_guard<SpinMutex> lock(const_cast<SpinMutex&>(log_metas_mutex_));
            return global_log_metas_.size();
        }

        // 解码失败总数（含因限流未输出到日志文件的部分）。
        // 非零说明有 formatter 抛异常或格式串与参数不匹配，值得排查。
        uint64_t decode_error_count() const {
            return decode_error_count_ref().load(std::memory_order_relaxed);
        }

        // IO 丢弃字节总数：appender 持续写失败（如磁盘满）且保留 tail 超过半缓冲
        // 时被丢弃的字节。非零说明输出目的地异常，值得排查。
        uint64_t io_drop_bytes() const {
            return io_drop_bytes_ref().load(std::memory_order_relaxed);
        }

        //崩溃状态标识
        std::atomic<bool> is_crashed_{ false };
        std::atomic<bool> crash_drain_done_{ false };
        // 崩溃路径专用的 ns/tick 缓存：在 install_crash_handler() 的正常上下文
        // 预先算好，供信号处理器内的 emergency_flush() 使用（信号处理中不能
        // 调用可能加锁/走系统调用的计时接口）。
        double crash_ns_per_tick_{ 0.0 };
    };

    inline ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer* NanoLogger::thread_buffer_ = nullptr;
    inline thread_local NanoLogger::ThreadBufferDestroyer NanoLogger::thread_buffer_destroyer_;

    // ---------------------------------------------------------------------------
    // 崩溃信号捕获 (Crash Handler)
    // ---------------------------------------------------------------------------
    namespace detail {
        // 预先缓存的实例指针与超时参数：信号处理器内不得调用
        // NanoLogger::instance()（函数局部 static 的 guard 可能加锁），
        // 只能通过这个在正常上下文写好的裸指针访问。
        inline NanoLogger*& crash_logger_ptr() {
            static NanoLogger* p = nullptr;
            return p;
        }

        inline void crash_signal_handler(int sig) {
            // 仅执行 async-signal-safe 动作：置原子标志 + rdtsc 忙等排空。
            if (NanoLogger* logger = crash_logger_ptr()) {
                logger->emergency_flush();
            }

            // 恢复系统默认行为并重新抛出，以生成 Core Dump
            std::signal(sig, SIG_DFL);
            std::raise(sig);
        }
    }

    inline void install_crash_handler() {
        // 在正常上下文完成所有可能加锁/走系统调用的准备工作：
        // 1) 触发单例构造（避免崩溃时才首次构造）
        // 2) 缓存实例指针供信号处理器使用
        // 3) 缓存 ns/tick，避免信号处理中调用 steady_clock
        NanoLogger& logger = NanoLogger::instance();
        detail::crash_logger_ptr() = &logger;
        logger.cache_crash_clock();

        std::signal(SIGSEGV, detail::crash_signal_handler);
        std::signal(SIGABRT, detail::crash_signal_handler);
        std::signal(SIGFPE, detail::crash_signal_handler);
        std::signal(SIGILL, detail::crash_signal_handler);
    }

}  //end namespace zrlog

// ---------------------------------------------------------------------------
// 静态宏：编译期极致优化
// ---------------------------------------------------------------------------

// 确保 format 是字符串字面量的魔法技巧：
#define ZRLOG_ENSURE_STRING_LITERAL(fmt_str)  "" fmt_str ""

#define ZRLOG_INIT_CONF(config) zrlog::NanoLogger::instance().init(config)
#define ZRLOG_INIT(filename, level) zrlog::NanoLogger::instance().init(filename, level)
#define ZRLOG_FINI() zrlog::NanoLogger::instance().fini()

#define ZRLOG_BODY(level, format, ...)                                                              \
    do {                                                                                            \
        /* 如果 format 是变量，"" format "" 这一步就会报清晰的错误： */                                \
        /* "error: expected ';' before 'format'" -> 提醒用户不要传变量 */                            \
        constexpr const char* _check_literal = ZRLOG_ENSURE_STRING_LITERAL(format);                 \
        (void)_check_literal;                                                                       \
                                                                                                    \
        zrlog::NanoLogger &logger = zrlog::NanoLogger::instance();                                  \
        {                                                                                           \
            static std::atomic<uint32_t> log_id{ 0 };                                               \
            struct FmtProvider {                                                                    \
                static constexpr auto compile() {                                                   \
                    /* 将固定日志头与用户日志格式完美拼接 */                                           \
                    return FMT_COMPILE(ZRLOG_HEADER_FMT format);                                    \
                }                                                                                   \
            };                                                                                      \
            std::string_view file { __FILE__, sizeof(__FILE__) - 1 };                               \
            std::string_view func { __func__, sizeof(__func__) - 1 };                               \
            logger.log<FmtProvider>(log_id, level, file, __LINE__, func, ##__VA_ARGS__);            \
        }                                                                                           \
    } while (0)

#define ZRLOG_TRACE(format, ...) ZRLOG_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DEBUG(format, ...) ZRLOG_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_INFO(format, ...)  ZRLOG_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_WARN(format, ...)  ZRLOG_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_ERROR(format, ...) ZRLOG_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_FATAL(format, ...) ZRLOG_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// 动态宏：支持运行时生成的 format
// ---------------------------------------------------------------------------
#define ZRLOG_DYN_BODY(level, format, ...)                                                          \
    do {                                                                                            \
        zrlog::NanoLogger &logger = zrlog::NanoLogger::instance();                                  \
        {                                                                                           \
            /* 高 32 位 log_id | 低 32 位格式串哈希，单原子量保证一致快照 */                            \
            static std::atomic<uint64_t> dyn_state{ 0 };                                            \
            std::string_view file { __FILE__, sizeof(__FILE__) - 1 };                               \
            std::string_view func { __func__, sizeof(__func__) - 1 };                               \
            logger.log_runtime(dyn_state, level, file, __LINE__, func, format, ##__VA_ARGS__);      \
        }                                                                                           \
    } while (0)

#define ZRLOG_DYN_TRACE(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DYN_DEBUG(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_DYN_INFO(format, ...)  ZRLOG_DYN_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_DYN_WARN(format, ...)  ZRLOG_DYN_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_DYN_ERROR(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_DYN_FATAL(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)