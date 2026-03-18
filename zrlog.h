#pragma once

// The zrlog library version in the form major * 10000 + minor * 100 + patch.
// 如：2.4.2
#define ZRLOG_VERSION 20402

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

#include <vector>
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
#include <cstdio>
#include <cinttypes>
#include <ctime>
#include <charconv>
#include <memory>
#include <new>
#include <csignal>        // 用于崩溃信号捕获

// 获取当前 CPU 架构的 L1 Cache Line 大小 (C++17)
#ifdef __cpp_lib_hardware_interference_size
constexpr size_t ZRLOG_CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
constexpr size_t ZRLOG_CACHE_LINE_SIZE = 64; // 兼容老编译器的 Fallback
#endif

// 跨平台分支预测宏
#if defined(__GNUC__) || defined(__clang__)
#define ZRLOG_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ZRLOG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    // MSVC 或其他编译器不提供静态预测，直接返回表达式本身
#define ZRLOG_LIKELY(x)   (x)
#define ZRLOG_UNLIKELY(x) (x)
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
#define ZRLOG_CPU_PAUSE()               _mm_pause()
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

inline uint64_t zrlog_rdtsc() {
    return __rdtsc();
}

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

    // 包装函数
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
        template <typename T>
        static auto decode_arg(char*& ptr) -> typename decode_type<std::decay_t<T>>::type {
            using DecayedT = std::decay_t<T>;

            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                zrlog::string_literal_t sl;
                std::memcpy(&sl, ptr, sizeof(zrlog::string_literal_t));
                ptr += sizeof(zrlog::string_literal_t);
                return sl;
            }
            else if constexpr (std::is_same_v<DecayedT, const char*> || std::is_same_v<DecayedT, char*> ||
                std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                uint32_t len;
                std::memcpy(&len, ptr, sizeof(uint32_t));
                const char* str = ptr + sizeof(uint32_t);
                ptr += sizeof(uint32_t) + len;
                return std::string_view(str, len);
            }
            else {
                DecayedT val;
                std::memcpy(&val, ptr, sizeof(DecayedT));
                ptr += sizeof(DecayedT);
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
            uint64_t tsc = zrlog_rdtsc();
            uint32_t seq;
            Anchor   anc;
            do {
                seq = seq_.load(std::memory_order_acquire);
                anc = anchor_;
                std::atomic_thread_fence(std::memory_order_acquire);
            } while (seq != seq_.load(std::memory_order_relaxed) || (seq & 1));

            return anc.base_ns + tsc2ns(tsc - anc.base_tsc);
        }

        inline uint64_t current_time_ns_i() const {
            return anchor_.base_ns + tsc2ns(zrlog_rdtsc() - anchor_.base_tsc);
        }

        inline uint64_t tsc2ns(uint64_t tsc_diff) const {
#if defined(_MSC_VER) && defined(_M_X64)
            unsigned __int64 high;
            unsigned __int64 low = _umul128(tsc_diff, multiplier_, &high);
            return (high << (64 - shift_)) | (low >> shift_);
#elif defined(__SIZEOF_INT128__)
            return (uint64_t)((unsigned __int128)tsc_diff * multiplier_ >> shift_);
#else
            return static_cast<uint64_t>(tsc_diff * multiplier_d_);
#endif
        }

        bool calibrate(int rounds = 3, int interval_ms = 1) {
            std::vector<double> rates;
            rates.reserve(rounds);

            for (int i = 0; i < rounds; ++i) {
                auto t0 = std::chrono::system_clock::now();
                uint64_t tsc0 = zrlog_rdtsc();
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                auto t1 = std::chrono::system_clock::now();
                uint64_t tsc1 = zrlog_rdtsc();

                uint64_t ns0 = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
                uint64_t ns1 = std::chrono::duration_cast<std::chrono::nanoseconds>(t1.time_since_epoch()).count();
                double ns_delta = static_cast<double>(ns1 - ns0);
                double tsc_delta = static_cast<double>(tsc1 - tsc0);
                if (tsc_delta > 0) {
                    rates.push_back(ns_delta / tsc_delta);
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
#if !defined(_MSC_VER) || !defined(_M_X64)
#if !defined(__SIZEOF_INT128__)
            multiplier_d_ = rate;
#endif
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

        void sync_anchor_unlocked() {
            auto t = std::chrono::system_clock::now();
            anchor_.base_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
            anchor_.base_tsc = zrlog_rdtsc();
        }

        static constexpr int shift_ = 32;
        uint64_t multiplier_ = 0;
#if !defined(_MSC_VER) || !defined(_M_X64)
#if !defined(__SIZEOF_INT128__)
        double multiplier_d_ = 1.0;
#endif
#endif
        std::atomic<uint32_t> seq_{ 0 };
        Anchor anchor_{ 0, 0 };
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
        uint32_t idle_wait_interval_us = 1000;          //空闲等待间隔(微秒)
        uint32_t force_flush_interval_ms = 3000;        //强制刷盘间隔(毫秒)
        uint32_t crash_drain_wait_ms = 3000;            //发生崩溃的业务线程自旋阻塞，给后台线程抢救数据的时长(毫秒)
        bool     enable_huge_page = false;              //启用OS的HugePage内存分配

        // 支持将日志后台线程绑定到多个核心 (如 {0, 1, 2})
        // 默认空列表表示由 OS 全局调度
        std::vector<int> background_thread_core_ids;

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
            char* ptr = const_cast<char*>(data);
            size_t nleft = len;
            int ret;

            do {
                ret = write(ptr, nleft);
                if (ret > 0) {
                    nleft -= ret;
                    ptr += ret;
                }
                else if (ret < 0) {
                    if (errno == EINTR || errno == EAGAIN) {
                        continue;
                    }
                    break;
                }
                else {
                    break;
                }
            } while (nleft > 0);

            return (len - nleft);
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
            int ret = ZRLOG_WRITE(fd_, data, len);
            if (ret > 0) {
                current_size_ += ret;
            }
            else if (ret < 0) {
                // 【修复 BUG】：写失败 (如 ENOSPC 磁盘满)。
                // 虽然 fd 还开着，但数据写不进去了！必须立刻兜底到 stderr 防止数据丢失。
                [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, data, len);

                // 强制累加 current_size_，确保后续能触发 roll() 尝试重新整理文件系统状态
                current_size_ += len;
            }
            return ret;
        }

        int flush() override {
            if (ZRLOG_LIKELY(fd_ != -1)) {
                return ZRLOG_FLUSH_FILE(fd_);
            }
            return 0;
        }

    private:
        void open_current_file() {
            fd_ = ZRLOG_OPEN(base_path_.c_str(), ZRLOG_O_FLAGS, ZRLOG_S_FLAGS);
            if (fd_ != -1) {
                struct stat st;
                if (fstat(fd_, &st) == 0) {
                    current_size_ = st.st_size;
                }
                else {
                    current_size_ = 0;
                }
                next_roll_size_ = max_size_;
            }
            else {
                // 文件打开失败的严重报警与退避设定
                const char err_msg[] = "\n[ZRLOG CRITICAL] Failed to open log file! Fallback to stderr.\n";
                [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg)-1);
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
                    // 【修复 BUG】：真正的纯栈内存分配 (避免 std::string 触发 malloc)
                    // 4096 (PATH_MAX) 足够容纳任何合法的文件路径
                    char src_buf[4096];
                    char dst_buf[4096];

                    if (base_path_.size() + 16 < sizeof(src_buf)) {
                        size_t base_len = base_path_.size();
                        std::memcpy(src_buf, base_path_.data(), base_len);
                        std::memcpy(dst_buf, base_path_.data(), base_len);

                        for (uint32_t i = max_files_ - 1; i >= 1; --i) {
                            auto src_res = fmt::format_to(src_buf + base_len, ".{}", i);
                            *src_res = '\0';

                            if (ZRLOG_ACCESS(src_buf, ZRLOG_F_OK) != 0) {
                                continue;
                            }

                            auto dst_res = fmt::format_to(dst_buf + base_len, ".{}", i + 1);
                            *dst_res = '\0';

                            std::remove(dst_buf);
                            std::rename(src_buf, dst_buf);
                        }

                        auto dst_res = fmt::format_to(dst_buf + base_len, ".1");
                        *dst_res = '\0';
                        std::remove(dst_buf);

                        if (std::rename(base_path_.c_str(), dst_buf) != 0) {
                            rename_success = false;
                            const char err_msg[] = "\n[ZRLOG ERROR] Rotate failed! Appending to existing file.\n";
                            [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg)-1);
                        }
                    }
                }
            }
            else {
                // 【修复 BUG】：当 max_files_ == 0 时，绝不能保留原文件，否则死循环。
                // 必须无条件删除，让 open_current_file() 重头创建一个 0 字节的新文件。
                std::remove(base_path_.c_str());
            }

            open_current_file();

            // 退避策略：如果重命名失败且我们还在往老文件里写，延迟下一次重试时间
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
        class IOBuffer;
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

        // 【极限压缩】将 LogEntryHeader 压缩至 16 字节，大幅提升缓存命中率与容量
        struct LogEntryHeader {
            uint32_t total_size;
            uint32_t log_id;
            uint64_t time;
        };

        class IOBuffer {
        public:
            IOBuffer(ILogAppender* appender, uint32_t size) : appender_(appender), size_(size) {
                if (size_ < 1024) {
                    size_ = 1024;
                }
                data_.resize(size_);
            }

            void append(const char* src, size_t len) {
                if (ZRLOG_UNLIKELY(pos_ + len > size_)) {
                    flush_to_os();
                }
                if (ZRLOG_UNLIKELY(len > size_)) {
                    appender_->writen(src, len);
                    return;
                }
                std::memcpy(data_.data() + pos_, src, len);
                pos_ += len;
            }

            void flush_to_os() {
                if (ZRLOG_LIKELY(pos_ > 0)) {
                    appender_->writen(data_.data(), pos_);
                    pos_ = 0;
                }
            }

            void flush_force() {
                flush_to_os();
                appender_->flush();
            }

            char* current_ptr() {
                return data_.data() + pos_;
            }
            size_t available_size() const {
                return size_ - pos_;
            }
            void advance(size_t len) {
                pos_ += len;
            }

        private:
            ILogAppender* appender_ = nullptr;
            std::vector<char> data_;
            size_t size_;
            size_t pos_ = 0;
        };

        static NanoLogger& instance() {
            static NanoLogger logger;
            return logger;
        }

        bool init(const Config& config) {
            config_ = config;

            if (!log_thread_running_.load(std::memory_order_relaxed)) {
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
                    idle_wait_condition_.notify_one();
                    log_thread_.join();
                }
                appender_.reset();
            }
        }

        inline bool check_level(LogLevel level) const {
            return level >= config_.level;
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

            uint32_t log_id = log_id_atom.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(0 == log_id)) {
                log_id = register_log_meta<FmtProvider, typename std::decay<Args>::type...>(
                    log_id_atom, level, file, line, func);
            }

            return push_log_entry(log_id, std::forward<Args>(args)...);
        }

        // ========================================================================
        // Frontend: 动态日志入口 (支持运行时 std::string format)
        // ========================================================================
        template<typename... Args>
        bool log_runtime(std::atomic<uint32_t>& log_id_atom, LogLevel level, std::string_view file, uint32_t line,
            std::string_view func, std::string_view format, Args&&... args) {

            // 在编译期扫描所有参数，如果有裸指针，直接让编译失败！
            static_assert((... && !detail::is_raw_char_ptr_v<Args>),
                "[ZRLOG FATAL ERROR] Raw char* / const char* is strictly forbidden for performance and safety! "
                "Please use std::string_view(ptr, len) or zrlog::literal instead.");

            uint32_t log_id = log_id_atom.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(0 == log_id)) {
                log_id = register_runtime_log_meta<typename std::decay<Args>::type...>(
                    log_id_atom, level, file, line, func, format);
            }

            return push_log_entry(log_id, std::forward<Args>(args)...);
        }

        // 崩溃兜底紧急刷盘接口
        void emergency_flush() {
            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                return;
            }

            // 标记为崩溃模式，并通知后台消费者立刻醒来
            is_crashed_.store(true, std::memory_order_release);
            //notify_consumer();

            // 当前线程（发生崩溃的业务线程）自旋阻塞，给后台线程抢救数据的时间。
            // 设置 crash_drain_wait_ms 超时，防止磁盘卡死导致进程无法退出 Core Dump。
            auto start_time = std::chrono::steady_clock::now();
            while (!crash_drain_done_.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() - start_time > std::chrono::milliseconds(config_.crash_drain_wait_ms)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

    private:

        // ========================================================================
        // 公共的入队与序列化逻辑 (被 log 和 log_runtime 共同调用，完美转发)
        // ========================================================================
        template<typename... Args>
        inline bool push_log_entry(uint32_t log_id, Args&&... args) {
            ThreadBuffer *buffer = get_thread_buffer();
            if (ZRLOG_UNLIKELY(nullptr == buffer)) {
                return false;
            }

            uint32_t total_size = sizeof(LogEntryHeader) + calculate_args_size_all(args...);
            //内存地址对齐,所在将总长度向上对齐到 8 字节！
            uint32_t aligned_len = (total_size + 7) & ~7u;

            char *ptr = buffer->alloc(aligned_len);
            if (ZRLOG_UNLIKELY(nullptr == ptr)) {
                switch (config_.buffer_full_policy) {
                case BufferFullPolicy::Discard:
                    return false;

                case BufferFullPolicy::Block: {
                    notify_consumer();
                    uint32_t spin_count = 0;
                    do {
                        if (!log_thread_running_.load(std::memory_order_relaxed)) {
                            return false;
                        }
                        if (spin_count < 256) {
                            ZRLOG_CPU_PAUSE();
                        }
                        else {
                            std::this_thread::yield();
                        }
                        ++spin_count;
                        ptr = buffer->alloc(aligned_len);
                    } while (ZRLOG_UNLIKELY(nullptr == ptr));
                }
                break;

                case BufferFullPolicy::Retry: {
                    notify_consumer();
                    uint32_t spin_count = 0;
                    do {
                        if (!log_thread_running_.load(std::memory_order_relaxed)) {
                            return false;
                        }
                        if (spin_count >= config_.buffer_full_retry_count) {
                            return false;
                        }
                        if (spin_count < 256) {
                            ZRLOG_CPU_PAUSE();
                        }
                        else {
                            std::this_thread::yield();
                        }
                        ++spin_count;
                        ptr = buffer->alloc(aligned_len);
                    } while (ZRLOG_UNLIKELY(nullptr == ptr));
                }
                break;

                default:
                    return false;
                }
            }

            // 分配成功，进行真正的极速序列化
            LogEntryHeader *header = reinterpret_cast<LogEntryHeader *>(ptr);
            header->total_size = aligned_len;
            header->log_id = log_id;
            header->time = TscClock::now_ns_i();
            serialize_args_all(ptr + sizeof(LogEntryHeader), std::forward<Args>(args)...);
            buffer->commit(aligned_len);
            notify_consumer();

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
            const char *time_str = get_time_format_cache(header.time, nano);

            // 用大括号初始化列表保证严格的从左到右求值顺序，同时彻底消灭默认构造！
            std::apply([&](const auto&... final_args) {
                size_t space = out.available_size() - 1;  // 预留 1 字节给 \n
                // 一次性索要充足空间，消除格式化期间的多次空间检查
                if (ZRLOG_UNLIKELY(space < 2048)) {
                    out.flush_to_os();
                    space = out.available_size() -1;
                }

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

            }, std::tuple<typename detail::decode_type<Args>::type...>{ detail::decode_arg<Args>(buffer)... });
        }

        // ========================================================================
        // 动态日志 Decoder: 头部编译期极速渲染，正文使用链式写入
        // ========================================================================
        template<typename... Args>
        static void generated_runtime_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out) {
            uint32_t nano;
            const char* time_str = get_time_format_cache(header.time, nano);

            std::apply([&](const auto&... final_args) {
                size_t space = out.available_size() - 1; // 预留 1 字节给 \n
                // 一次性索要充足空间，消除格式化期间的多次空间检查
                if (ZRLOG_UNLIKELY(space < 2048)) {
                    out.flush_to_os();
                    space = out.available_size() - 1;
                }

                try {
                    // 1. 【第一次尝试：极速渲染头部】
                    auto hdr_res = fmt::format_to_n(out.current_ptr(), space,
                        FMT_COMPILE(ZRLOG_HEADER_FMT),
                        fmt::string_view(time_str, 19), nano,
                        loglevel_to_string(meta.level), thread_id,
                        meta.file, meta.line);

                    // 严格区分实际写入大小和理论需要大小
                    size_t actual_hdr_len = std::min<size_t>(hdr_res.size, space);
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
                    // 对于异常捕获，同样保留 1 字节并安全换行
                    size_t err_space = out.available_size() - 1;
                    auto err_msg = fmt::format_to_n(out.current_ptr(), err_space,
                        FMT_COMPILE("[FMT_ERROR] {}.{:09d} {} {}:{} format: '{}' error: '{}'"),
                        fmt::string_view(time_str, 19), nano, loglevel_to_string(meta.level), meta.file, meta.line,
                        fmt::string_view(meta.format.data(), meta.format.size()), e.what());

                    size_t actual_err_len = std::min<size_t>(err_msg.size, err_space);
                    out.current_ptr()[actual_err_len] = '\n';
                    out.advance(actual_err_len + 1);
                }
            }, std::tuple<typename detail::decode_type<Args>::type...>{ detail::decode_arg<Args>(buffer)... });
        }

        class ThreadBuffer {
        public:
            explicit ThreadBuffer(uint32_t size, uint64_t tid, bool enable_huge_page) : size_(normalize_size(size)), thread_id_(tid) {
                mask_ = size_ - 1;
                mem_block_ = util::allocate_block(size_, enable_huge_page);

                // 【按页预热 Fast Page Pre-faulting】
                // 如果成功吃到了大页，内核已经锁定了连续物理内存，无需预热。
                // 只有在降级到普通 4KB 内存时，才执行原版的强制映射循环。
                if (!mem_block_.is_huge_page) {
                    volatile char *p = mem_block_.ptr;
                    for (size_t i = 0; i < size_; i += 4096) {
                        p[i] = 0;
                    }
                }
            }

            ~ThreadBuffer() {
                zrlog::util::free_block(mem_block_);
            }

            inline uint64_t thread_id() const {
                return thread_id_;
            }

            // ------------------------------------------------------------------------
            // 前端（生产者）接口
            // ------------------------------------------------------------------------
            char* alloc(uint32_t len) {
                uint64_t w = write_index_.load(std::memory_order_relaxed);

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

                // 物理尾部连续空间足够，直接分配
                if (ZRLOG_LIKELY(len <= tail_free)) {
                    return mem_block_.ptr + phys_w;
                }

                // 物理尾部空间不够，必须绕回 (Wrap around)
                // 再次检查逻辑空间 (加上绕回产生的 padding 浪费后) 是否够用
                if (ZRLOG_UNLIKELY(w + tail_free + len - cached_read_index_ > size_)) {
                    cached_read_index_ = read_index_.load(std::memory_order_acquire);
                    if (ZRLOG_UNLIKELY(w + tail_free + len - cached_read_index_ > size_)) {
                        return nullptr;
                    }
                }

                constexpr uint32_t HEADER_SIZE = sizeof(LogEntryHeader);

                // 写入 Padding 占位符
                if (ZRLOG_LIKELY(tail_free >= HEADER_SIZE)) {
                    write_padding_local(phys_w, tail_free);
                }

                // 推进逻辑写下标，补齐到下一圈的物理 0 处
                w += tail_free;

                // 提前发布 Padding，让消费者可以看到绕回动作
                write_index_.store(w, std::memory_order_release);

                return mem_block_.ptr;
            }

            inline void commit(uint32_t len) {
                uint64_t w = write_index_.load(std::memory_order_relaxed);
                w += len;
                write_index_.store(w, std::memory_order_release);
            }

            // ------------------------------------------------------------------------
            // 后台（消费者）接口
            // ------------------------------------------------------------------------
            LogEntryHeader* try_read() {
                // 【消费者全本地化】
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
                    constexpr uint32_t HEADER_SIZE = sizeof(LogEntryHeader);

                    // 1. 隐式 Padding：尾部连 Header 都读不全
                    if (ZRLOG_UNLIKELY(tail_avail < HEADER_SIZE)) {
                        r += tail_avail;
                        local_read_index_ = r;
                        // 绕回时为了防止生产者卡死，强制推送一次游标
                        read_index_.store(r, std::memory_order_release);
                        continue;
                    }

                    LogEntryHeader *header = reinterpret_cast<LogEntryHeader *>(mem_block_.ptr + phys_r);

                    // 2. 显式 Padding
                    if (ZRLOG_UNLIKELY(header->log_id == PADDING_ID)) {
                        uint32_t claimed = header->total_size;
                        if (ZRLOG_UNLIKELY(claimed < HEADER_SIZE || claimed > tail_avail)) {
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
                    if (ZRLOG_UNLIKELY(r + header->total_size > cached_write_index_)) {
                        cached_write_index_ = write_index_.load(std::memory_order_acquire);
                        if (ZRLOG_UNLIKELY(r + header->total_size > cached_write_index_)) {
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
                return should_deallocate_;
            }
            inline void mark_deallocate() {
                should_deallocate_ = true;
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
            static const uint32_t PADDING_ID = 0xFFFFFFFF;

            static uint32_t normalize_size(uint32_t n) {
                constexpr uint32_t MIN_SIZE = 1024, MAX_POW2 = 1u << 30;
                if (n < MIN_SIZE) {
                    n = MIN_SIZE;
                }
                n--;
                n |= n >> 1;
                n |= n >> 2;
                n |= n >> 4;
                n |= n >> 8;
                n |= n >> 16;
                n++;
                if (n == 0) {
                    n = MIN_SIZE;
                }
                if (n > MAX_POW2) {
                    n = MAX_POW2;
                }
                return n;
            }

            inline void write_padding_local(uint32_t pos, uint32_t pad_size) {
                LogEntryHeader *p = reinterpret_cast<LogEntryHeader *>(mem_block_.ptr + pos);
                p->log_id = PADDING_ID;
                p->total_size = pad_size;
                p->time = 0;
            }

            // =========================================================================
            // 【极其严格的缓存行物理隔离】
            // C++17 alignas 将自动填充字节，彻底杜绝 False Sharing
            // =========================================================================

            // --- 生产者独占缓存行 (写入端) ---
            // 内存对齐并独占一行，只有前端线程会高频读写这里的变量
            alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
            uint64_t cached_read_index_{ 0 };

            // --- 消费者独占缓存行 (读取端) ---
            // 物理隔离！只有后台消费者线程会高频读写这里的变量
            alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
            uint64_t cached_write_index_{ 0 };
            uint64_t local_read_index_{ 0 };

            // --- 其他共享元数据 ---
            // 独立在一行，防止被游标的高频变动波及
            alignas(ZRLOG_CACHE_LINE_SIZE) uint32_t size_;
            uint32_t mask_;
            uint64_t thread_id_;

            // ========================================================================
            // [核心修改]：替换 原来的std::vector，使用原生指针与内存块元数据
            // ========================================================================
            util::MemoryBlock mem_block_;

            bool should_deallocate_{ false };
        };

        class ThreadBufferDestroyer {
        public:
            explicit ThreadBufferDestroyer() {
            }

            ~ThreadBufferDestroyer() {
                if (nullptr != NanoLogger::thread_buffer_) {
                    NanoLogger::thread_buffer_->mark_deallocate();
                    logger_->notify_consumer();
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

            log_id_atom.store(new_id, std::memory_order_relaxed);
            return new_id;
        }

        template<typename... Args>
        uint32_t register_runtime_log_meta(std::atomic<uint32_t>& log_id_atom, LogLevel level, 
            std::string_view file, uint32_t line, std::string_view func, std::string_view format) {
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
            // 运行时注册深拷贝 format，用于后续的 fmt::runtime
            new_log.format = std::string(format);
            new_log.decoder = &generated_runtime_decoder<Args...>;
            global_log_metas_.push_back(std::move(new_log));

            log_id_atom.store(new_id, std::memory_order_relaxed);
            return new_id;
        }

        ThreadBuffer* get_thread_buffer() {
            if (ZRLOG_UNLIKELY(nullptr == thread_buffer_)) {
                thread_buffer_ = new ThreadBuffer(config_.thread_buffer_size, util::get_thread_id(), config_.enable_huge_page);
                thread_buffer_destroyer_.init(this);
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                thread_buffers_.push_back(thread_buffer_);
            }
            return thread_buffer_;
        }

        size_t consume_buffers_round_robin(std::vector<LogMeta>& local_log_metas, IOBuffer& io_buf, bool full_drain = false) {
            size_t processed_count = 0;

            {
                // 追加日志元数据到本地集合
                std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                if (global_log_metas_.size() > local_log_metas.size()) {
                    local_log_metas.insert(local_log_metas.end(),
                        global_log_metas_.begin() + local_log_metas.size(),
                        global_log_metas_.end());
                }
            }
            {
                // 【连续内存预取】将新增的 Buffer 高效转移到后台集合
                // 注意：新加入的 Buffer 都在 vector 尾部，天生属于“非活跃区”，
                // 下一次纪元扫描时如果有数据，自然会被提拔到前面。
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                if (!thread_buffers_.empty()) {
                    thread_buffers_bg_.insert(thread_buffers_bg_.end(), thread_buffers_.begin(), thread_buffers_.end());
                    thread_buffers_.clear();
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
                    stat_consume_count_.fetch_add(1, std::memory_order_relaxed);
                    ++processed_count;
                    --quota;

                    if (header->log_id > 0 && header->log_id < local_log_metas.size()) {
                        const LogMeta& meta = local_log_metas[header->log_id];
                        char* args_ptr = (char*)header + sizeof(LogEntryHeader);
                        meta.decoder(args_ptr, meta, *header, tb->thread_id(), io_buf);
                        stat_consume_valid_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                    else {  // header->log_id 无效时，尝试再次拉取元数据
                        {
                            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                            if (global_log_metas_.size() > local_log_metas.size()) {
                                local_log_metas.insert(local_log_metas.end(),
                                    global_log_metas_.begin() + local_log_metas.size(),
                                    global_log_metas_.end());
                            }
                        }

                        if (header->log_id > 0 && header->log_id < local_log_metas.size()) {
                            const LogMeta& meta = local_log_metas[header->log_id];
                            char* args_ptr = (char*)header + sizeof(LogEntryHeader);
                            meta.decoder(args_ptr, meta, *header, tb->thread_id(), io_buf);
                            stat_consume_valid_count_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    tb->consume(header->total_size);
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
            if (!config_.background_thread_core_ids.empty()) {
                if (!util::pin_thread_to_cores(config_.background_thread_core_ids)) {
                    const char err_msg[] = "[ZRLOG WARNING] Failed to pin background thread to specified CPU cores.\n";
                    [[maybe_unused]] auto res = ZRLOG_WRITE(STDERR_FILENO, err_msg, sizeof(err_msg)-1);
                }
            }

            std::vector<LogMeta> local_log_metas;
            local_log_metas.reserve(1000);
            IOBuffer io_buf(appender_.get(), config_.io_buffer_size);

            auto last_force_flush_time = std::chrono::steady_clock::now();            
            while (log_thread_running_.load(std::memory_order_relaxed)) {

                // 【崩溃检测】发现发生严重崩溃，立即启动抢救式全量 Drain
                if (ZRLOG_UNLIKELY(is_crashed_.load(std::memory_order_acquire))) {
                    while (consume_buffers_round_robin(local_log_metas, io_buf, true) > 0) {
                    }
                    io_buf.flush_force();
                    crash_drain_done_.store(true, std::memory_order_release);
                    return; // 抢救完毕，安全退出
                }

                size_t process_count = consume_buffers_round_robin(local_log_metas, io_buf, false);

                // ========================================================================
                // 周期性物理落盘(防断电/内核崩溃)
                // ========================================================================
                // 无论系统是处于空闲还是极端高负载，间隔force_flush_interval时长必然触发一次真实的 fsync
                // 这保证了在面临机柜断电等最高级别物理灾难时，最多只丢失 force_flush_interval 毫秒的日志
                auto now = std::chrono::steady_clock::now();
                auto force_flush_interval = std::chrono::milliseconds(config_.force_flush_interval_ms);
                if (ZRLOG_UNLIKELY(now - last_force_flush_time >= force_flush_interval)) {
                    TscClock::instance().sync_system_time();

                    io_buf.flush_force();           // 触发底层的 write + fsync
                    last_force_flush_time = now;
                }

                if (process_count < 1) {  // idle
                    // 空闲时，立刻将当前用户态缓冲交由操作系统 (Page Cache)
                    // 保证了低延迟情况下的 tail -f 实时可见性
                    io_buf.flush_to_os();

                    idle_wait_flag_.store(true, std::memory_order_relaxed);
                    {
                        std::unique_lock<std::mutex> lock(idle_wait_mutex_);
                        idle_wait_condition_.wait_for(lock, std::chrono::microseconds(config_.idle_wait_interval_us));
                    }
                    idle_wait_flag_.store(false, std::memory_order_relaxed);
                }
            }

            // ==================== shutdown 全量 drain（绕过 256 限制） ====================
            // 注意：full_drain = true 时，consume 内部的 scan_limit 会强制为 size()，保证彻底清空
            while (consume_buffers_round_robin(local_log_metas, io_buf, true) > 0) {
            }

            io_buf.flush_force();
        }

        inline void notify_consumer() {
            if (idle_wait_flag_.load(std::memory_order_relaxed)) {
                idle_wait_flag_.store(false, std::memory_order_relaxed);
                idle_wait_condition_.notify_one();
            }
        }

        NanoLogger() {
            TscClock::instance();
            thread_buffers_.reserve(100);
            thread_buffers_bg_.reserve(100);
            global_log_metas_.reserve(1000);

            //新增log_id为0的LogMeta项,保证有效log_id的序号从1开始
            global_log_metas_.emplace_back(std::move(LogMeta()));
        }
        ~NanoLogger() {
            fini();
        }

        static ZRLOG_FAST_THREAD_LOCAL ThreadBuffer* thread_buffer_;
        static thread_local ThreadBufferDestroyer thread_buffer_destroyer_;

        Config config_;
        SpinMutex log_metas_mutex_;
        std::vector<LogMeta> global_log_metas_;
        SpinMutex thread_buffers_mutex_;
        std::vector<ThreadBuffer*> thread_buffers_;
        std::vector<ThreadBuffer*> thread_buffers_bg_;

        size_t active_buffer_count_{ 0 };
        uint64_t poll_cycles_{ 0 };

        std::unique_ptr<ILogAppender> appender_;
        std::thread log_thread_;
        std::atomic<bool> log_thread_running_{ false };

        std::mutex              idle_wait_mutex_;
        std::condition_variable idle_wait_condition_;
        alignas(ZRLOG_CACHE_LINE_SIZE) std::atomic<bool> idle_wait_flag_{ false };

    public:
        //statistics
        std::atomic<uint64_t>  stat_produce_count_{ 0 };
        std::atomic<uint64_t>  stat_produce_valid_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_valid_count_{ 0 };

        //崩溃状态标识
        std::atomic<bool> is_crashed_{ false };
        std::atomic<bool> crash_drain_done_{ false };
    };

    inline ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer* NanoLogger::thread_buffer_ = nullptr;
    inline thread_local NanoLogger::ThreadBufferDestroyer NanoLogger::thread_buffer_destroyer_;

    // ---------------------------------------------------------------------------
    // 崩溃信号捕获 (Crash Handler)
    // ---------------------------------------------------------------------------
    namespace detail {
        inline void crash_signal_handler(int sig) {
            // 发生崩溃，触发日志紧急刷盘
            NanoLogger::instance().emergency_flush();

            // 恢复系统默认行为并重新抛出，以生成 Core Dump
            std::signal(sig, SIG_DFL);
            std::raise(sig);
        }
    }

    inline void install_crash_handler() {
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
        /* 如果 format 是变量，"" format "" 这一步就会报清晰的错误： */                             \
        /* "error: expected ';' before 'format'" -> 提醒用户不要传变量 */                           \
        constexpr const char* _check_literal = ZRLOG_ENSURE_STRING_LITERAL(format);                 \
        (void)_check_literal;                                                                       \
                                                                                                    \
        zrlog::NanoLogger &logger = zrlog::NanoLogger::instance();                                  \
        if (logger.check_level(level)) {                                                            \
            static std::atomic<uint32_t> log_id{0};                                                 \
            struct FmtProvider {                                                                    \
                static constexpr auto compile() {                                                   \
                    /* 将固定日志头与用户日志格式完美拼接 */                                        \
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
        if (logger.check_level(level)) {                                                            \
            static std::atomic<uint32_t> log_id{0};                                                 \
            std::string_view file { __FILE__, sizeof(__FILE__) - 1 };                               \
            std::string_view func { __func__, sizeof(__func__) - 1 };                               \
            logger.log_runtime(log_id, level, file, __LINE__, func, format, ##__VA_ARGS__);         \
        }                                                                                           \
    } while (0)

#define ZRLOG_DYN_TRACE(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DYN_DEBUG(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_DYN_INFO(format, ...)  ZRLOG_DYN_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_DYN_WARN(format, ...)  ZRLOG_DYN_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_DYN_ERROR(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_DYN_FATAL(format, ...) ZRLOG_DYN_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)