#pragma once

// The zrlog library version in the form major * 10000 + minor * 100 + patch.
// 如：2.1.0
#define ZRLOG_VERSION 20100

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

// 获取当前 CPU 架构的 L1 Cache Line 大小 (C++17)
#ifdef __cpp_lib_hardware_interference_size
constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
constexpr size_t CACHE_LINE_SIZE = 64; // 兼容老编译器的 Fallback
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
#define ZRLOG_O_FLAGS                   (_O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY)
#define ZRLOG_S_FLAGS                   (_S_IREAD | _S_IWRITE)
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

#ifdef __x86_64__
#include <x86intrin.h>
#endif

#define ZRLOG_OPEN(path, flags, mode)   ::open(path, flags, mode)
#define ZRLOG_WRITE(fd, data, len)      ::write(fd, data, len)
#define ZRLOG_CLOSE(fd)                 ::close(fd)
#define ZRLOG_FLUSH_FILE(fd)            ::fsync(fd)
#define ZRLOG_O_FLAGS                   (O_WRONLY | O_CREAT | O_APPEND)
#define ZRLOG_S_FLAGS                   (0644)
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
        const char *ptr;
        uint32_t len;
    };

    // 包装函数
    template <size_t N>
    inline constexpr string_literal_t literal(const char(&str)[N]) noexcept {
        // 安全转换指针为 uint64_t
        return {str, static_cast<uint32_t>(N - 1)};
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

        // 指针漂移提取器
        template <typename T>
        static auto decode_arg(char*& ptr) -> typename decode_type<std::decay_t<T>>::type {
            using DecayedT = std::decay_t<T>;

            // 直接读取string_literal_t结构体
            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                zrlog::string_literal_t sl;
                std::memcpy(&sl, ptr, sizeof(zrlog::string_literal_t));
                ptr += sizeof(zrlog::string_literal_t); 
                return sl; // 返回给 fmt，fmt 内部会自动调用我们上面写的 format_as
            }
            // 原有的深拷贝字符串提取逻辑
            else if constexpr (std::is_same_v<DecayedT, const char*> || std::is_same_v<DecayedT, char*> ||
                std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                uint32_t len;
                std::memcpy(&len, ptr, sizeof(uint32_t));
                const char* str = ptr + sizeof(uint32_t);
                ptr += sizeof(uint32_t) + len;
                return std::string_view(str, len > 0 ? len - 1 : 0);
            }
            // 基础类型提取逻辑
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
    }

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

        bool calibrate(int rounds = 5, int interval_ms = 20) {
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

    static inline uint64_t get_thread_id() {
        static thread_local uint64_t tid_ = []() -> uint64_t {
#ifdef  _WIN32
            return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
            return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
            return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
        }();
        return tid_;
    }

    enum class LogLevel : uint8_t {
        TRACE = 0, DEBUG, INFO, WARN, ERR, FATAL
    };
    inline const char* loglevel_to_string(LogLevel level) {
        static const char* names[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
        uint8_t idx = static_cast<uint8_t>(level);
        return (idx <= 5) ? names[idx] : "UNKNOWN";
    }

    enum class AppenderType : uint8_t {
        File,
        Console
    };

    enum class BufferFullPolicy : uint8_t {
        Discard = 0,        // 策略1：直接丢弃    (默认，保证业务线程绝对不被阻塞，极致低时延)
        Block = 1,          // 策略2：无限重试    (阻塞直到有空间，保证绝对不丢日志，但会导致业务卡顿)
        Retry = 2           // 策略3：有限次重试  (重试一定次数后仍满，则丢弃，兼顾平滑与低时延)
    };

    struct Config {
        AppenderType appender = AppenderType::File;
        std::string filename;
        LogLevel level = LogLevel::DEBUG;
        uint32_t io_buffer_size = 1024 * 1024 * 1;      //io缓冲大小(也即日志格式化缓冲, 全局唯一)
        uint32_t thread_buffer_size = 1024 * 1024 * 1;  //每个线程的缓冲大小(前端二进制序列化缓冲 测试发现越大如16M,时延也变大)
        uint32_t per_thread_quota = 256;                //每个线程的格式化日志的配额(防止线程产生日志太快,公平处理每个线程日志)
        uint32_t idle_wait_interval_us = 500;           //空闲等待间隔(微妙)

        // ---- 缓冲区满时的处理策略 ----
        BufferFullPolicy buffer_full_policy = BufferFullPolicy::Discard;
        uint32_t buffer_full_retry_count = 1024;     //重试次数(仅在 Retry 策略下生效)
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
        int fd_ = -1;
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
        // Decoder 签名新增 thread_id 参数，避免在每条 Header 中重复存储
        using DecoderFn = void(*)(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out);

        struct LogMeta {
            uint32_t    id;
            LogLevel    level;
            const char* file;
            uint32_t    line;
            const char* func;
            DecoderFn   decoder; // 格式化字符串已被 FMT_COMPILE 吸收，无需 std::string format
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
                    if (config_.filename.empty()) {
                        fprintf(stderr, "NanoLogger: File appender requires filename.\n");
                        return false;
                    }
                    auto file_appender = std::make_unique<FileLogAppender>(config_.filename);
                    if (!file_appender->is_open()) {
                        return false;
                    }
                    appender_ = std::move(file_appender);
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
        // Frontend: 接收由宏传入的 FmtProvider (包含编译期的 FMT_COMPILE 结构)
        // ========================================================================
        template<typename FmtProvider, typename... Args>
        bool log(std::atomic<uint32_t>& log_id_atom, LogLevel level, const char* file, uint32_t line,
            const char* func, Args&&... args) {

            uint32_t log_id = log_id_atom.load(std::memory_order_relaxed);
            if (ZRLOG_UNLIKELY(0 == log_id)) {
                // 将 FmtProvider 一同注入模板实例化
                log_id = register_log_meta<FmtProvider, typename std::decay<Args>::type...>(
                    log_id_atom, level, file, line, func);
            }

            ThreadBuffer* buffer = get_thread_buffer();
            if (ZRLOG_LIKELY(nullptr != buffer)) {
                uint32_t args_size = calculate_args_size_all(args...);
                uint32_t total_size = sizeof(LogEntryHeader) + args_size;
                char* ptr = buffer->alloc(total_size);

                if (ZRLOG_UNLIKELY(nullptr == ptr)) {
                    switch (config_.buffer_full_policy) {
                    case BufferFullPolicy::Discard:
                        return false;

                    case BufferFullPolicy::Block: {
                        // 兜底：如果后台消费者线程碰巧在休眠，强制唤醒它赶紧消费腾出空间
                        notify_consumer();

                        uint32_t spin_count = 0;
                        do {
                            // 如果后端线程已经停止，避免死循环
                            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                                return false;
                            }

                            // 智能退避算法 (Adaptive Backoff)
                            if (spin_count < 256) {
                                ZRLOG_CPU_PAUSE(); // 前256次仅做CPU级自旋，避免上下文切换的重负载
                            }
                            else {
                                std::this_thread::yield(); // 之后让出线程时间片，防止前端线程把CPU跑满100%导致消费者饿死
                            }

                            ++spin_count;
                            ptr = buffer->alloc(total_size); // 重新尝试分配
                        } while (ZRLOG_UNLIKELY(nullptr == ptr));
                    }
                    break;

                    case BufferFullPolicy::Retry: {
                        // 兜底：如果后台消费者线程碰巧在休眠，强制唤醒它赶紧消费腾出空间
                        notify_consumer();

                        uint32_t spin_count = 0;
                        do {
                            // 如果后端线程已经停止，避免死循环
                            if (!log_thread_running_.load(std::memory_order_relaxed)) {
                                return false;
                            }

                            if (spin_count >= config_.buffer_full_retry_count) {
                                return false; // 策略3：超过重试次数，最终丢弃
                            }

                            // 智能退避算法 (Adaptive Backoff)
                            if (spin_count < 256) {
                                ZRLOG_CPU_PAUSE(); // 前256次仅做CPU级自旋，避免上下文切换的重负载
                            }
                            else {
                                std::this_thread::yield(); // 之后让出线程时间片，防止前端线程把CPU跑满100%导致消费者饿死
                            }

                            ++spin_count;
                            ptr = buffer->alloc(total_size); // 重新尝试分配
                        } while (ZRLOG_UNLIKELY(nullptr == ptr));
                    }
                    break;

                    default:
                        return false;

                    }
                }

                // 如果走到这里，说明 ptr 必然不为 nullptr (分配成功)
                LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(ptr);
                header->total_size = total_size;
                header->log_id = log_id;
                header->time = TscClock::now_ns_i();
                serialize_args_all(ptr + sizeof(LogEntryHeader), std::forward<Args>(args)...);
                buffer->commit(total_size);
                notify_consumer();

                return true;
            }

            return false;
        }

    private:
        template <typename T>
        static uint32_t arg_size(const T& val) {
            using DecayedT = std::decay_t<T>;
            // 如果是静态字符串，只占用 16 字节的空间！彻底消除 O(N) 的 strlen 耗时
            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                return sizeof(zrlog::string_literal_t);
            }
            else if constexpr (std::is_same_v<DecayedT, const char*> || std::is_same_v<DecayedT, char*>) {
                return sizeof(uint32_t) + (val ? (uint32_t)strlen(val) + 1 : 1);
            }
            else if constexpr (std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                return sizeof(uint32_t) + (uint32_t)val.size() + 1;
            }
            else {
                return sizeof(T);
            }
        }

        // 拦截字符串常量池字面量，如 "hello"，直接用 N 计算，彻底消灭前端 strlen
        template <size_t N>
        static uint32_t arg_size(const char(&)[N]) {
            return sizeof(uint32_t) + N;
        }

        static uint32_t calculate_args_size_all() { 
            return 0; 
        }

        template <typename... Args>
        static uint32_t calculate_args_size_all(const Args&... args) {
            return (0 + ... + arg_size(args));
        }

        template <typename T>
        static void serialize_arg(char*& ptr, const T& val) {
            using DecayedT = std::decay_t<T>;
            
            if constexpr (std::is_same_v<DecayedT, zrlog::string_literal_t>) {
                std::memcpy(ptr, &val, sizeof(zrlog::string_literal_t));
                ptr += sizeof(zrlog::string_literal_t);
            }
            else if constexpr (std::is_same_v<DecayedT, const char*> || std::is_same_v<DecayedT, char*>) {
                uint32_t len = val ? (uint32_t)strlen(val) + 1 : 1;
                std::memcpy(ptr, &len, sizeof(uint32_t));
                if (val) {
                    std::memcpy(ptr + sizeof(uint32_t), val, len);
                }
                else {
                    *(ptr + sizeof(uint32_t)) = '\0';
                }
                ptr += sizeof(uint32_t) + len;
            }
            else if constexpr (std::is_same_v<DecayedT, std::string> || std::is_same_v<DecayedT, std::string_view>) {
                uint32_t len = (uint32_t)val.size() + 1;
                std::memcpy(ptr, &len, sizeof(uint32_t));
                if (len > 1) {
                    std::memcpy(ptr + sizeof(uint32_t), val.data(), len - 1);
                }
                *(ptr + sizeof(uint32_t) + len - 1) = '\0';
                ptr += sizeof(uint32_t) + len;
            }
            else {
                std::memcpy(ptr, &val, sizeof(T));
                ptr += sizeof(T);
            }
        }

        template <size_t N>
        static void serialize_arg(char*& ptr, const char(&val)[N]) {
            uint32_t len = N;
            std::memcpy(ptr, &len, sizeof(uint32_t));
            std::memcpy(ptr + sizeof(uint32_t), val, N); // includes '\0'
            ptr += sizeof(uint32_t) + N;
        }

        static void serialize_args_all(char*) {
        }

        template <typename... Args>
        static void serialize_args_all(char* ptr, const Args&... args) {
            (..., serialize_arg(ptr, args));
        }

        template<typename FmtProvider, typename... Args>
        static void generated_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, uint64_t thread_id, IOBuffer& out) {

            // 0. 获取缓存的秒级时间字符串 (19 bytes)
            static thread_local time_t cache_sec = 0;
            static thread_local char cache_str[20] = { 0 };
            time_t sec = static_cast<time_t>(header.time / 1000000000);
            if (sec != cache_sec) {
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
                cache_str[19] = '\0';
                cache_sec = sec;
            }
            uint32_t nano = static_cast<uint32_t>(header.time % 1000000000);

            // 1. 声明类型安全的空 tuple 容器 (其中的字符串类型已被转换为 string_view)
            std::tuple<typename detail::decode_type<Args>::type...> args_tuple;

            // 2. 定义提取 Lambda
            auto extract = [&](auto& arg) {
                arg = detail::decode_arg<std::remove_reference_t<decltype(arg)>>(buffer);
            };

            // 3. 用 C++17 逗号折叠表达式，依次填充 tuple 坑位 (完全没有 make_tuple 的深拷贝)
            std::apply([&](auto&... unpacked_args) {
                (..., extract(unpacked_args));
            }, args_tuple);

            // 4. 将填满的 tuple 平铺展开，直接送入 FMT_COMPILE 引擎！
            std::apply([&](const auto&... final_args) {
                size_t space = out.available_size();
                if (space < 1024) {
                    out.flush_to_os();
                    space = out.available_size();
                }

                // FmtProvider::compile() 提供了编译期的 AST (抽象语法树) 路由
                // final_args... 提供了绝对零拷贝的内存视图 (string_view)
                // 两者结合，fmt 库会生成最高效的汇编指令，直接将数据 memcpy 到 out.current_ptr()
                auto res = fmt::format_to_n(out.current_ptr(), space - 1, FmtProvider::compile(),
                    fmt::string_view(cache_str, 19), nano,
                    loglevel_to_string(meta.level), thread_id,
                    meta.file, meta.line, final_args...);

                // 处理缓冲区写满换行逻辑
                if (res.size >= space - 1) {
                    out.flush_to_os();
                    space = out.available_size();
                    res = fmt::format_to_n(out.current_ptr(), space - 1, FmtProvider::compile(),
                        fmt::string_view(cache_str, 19), nano,
                        loglevel_to_string(meta.level), thread_id,
                        meta.file, meta.line, final_args...);
                    size_t written = (res.size < space - 1) ? res.size : (space - 1);
                    out.current_ptr()[written] = '\n';
                    out.advance(written + 1);
                }
                else {
                    out.current_ptr()[res.size] = '\n';
                    out.advance(res.size + 1);
                }
            }, args_tuple);
        }

        class ThreadBuffer {
        public:
            explicit ThreadBuffer(uint32_t size, uint64_t tid) : size_(normalize_size(size)), thread_id_(tid) {
                mask_ = size_ - 1;
                buffer_.resize(size_);

                // 【按页预热 Fast Page Pre-faulting】
                // 每 4096 字节写一个 0，强迫操作系统映射所有物理页，比全量 memset 快几倍
                volatile char* p = buffer_.data();
                for (size_t i = 0; i < size_; i += 4096) {
                    p[i] = 0;
                }
            }

            ~ThreadBuffer() = default;

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
                    return buffer_.data() + phys_w;
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

                return buffer_.data();
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

                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(buffer_.data() + phys_r);

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
                LogEntryHeader* p = reinterpret_cast<LogEntryHeader*>(buffer_.data() + pos);
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
            alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
            uint64_t                 cached_read_index_{ 0 };

            // --- 消费者独占缓存行 (读取端) ---
            // 物理隔离！只有后台消费者线程会高频读写这里的变量
            alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
            uint64_t                 cached_write_index_{ 0 };
            uint64_t                 local_read_index_{ 0 };

            // --- 其他共享元数据 ---
            // 独立在一行，防止被游标的高频变动波及
            alignas(CACHE_LINE_SIZE) uint32_t size_;
            uint32_t                 mask_;
            uint64_t                 thread_id_;
            std::vector<char>        buffer_;
            bool                     should_deallocate_{ false };
        };

        class ThreadBufferDestroyer {
        public:
            explicit ThreadBufferDestroyer() {
            }

            ~ThreadBufferDestroyer() {
                if (nullptr != NanoLogger::thread_buffer_) {
                    NanoLogger::thread_buffer_->mark_deallocate();
                    NanoLogger::instance().notify_consumer();
                }
            }

            void init() {
            }
        };

        // ========================================================================
        // Management
        // ========================================================================
        template<typename FmtProvider, typename... Args>
        uint32_t register_log_meta(std::atomic<uint32_t>& log_id_atom, LogLevel level, const char* file, uint32_t line, const char* func) {
            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
            uint32_t id = log_id_atom.load(std::memory_order_relaxed);
            if (id != 0) {
                return id;
            }

            uint32_t new_id = static_cast<uint32_t>(global_log_metas_.size() + 1);
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

        ThreadBuffer* get_thread_buffer() {
            if ZRLOG_UNLIKELY(nullptr == thread_buffer_) {
                thread_buffer_ = new ThreadBuffer(config_.thread_buffer_size, get_thread_id());
                thread_buffer_destroyer_.init();
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

            LogEntryHeader *header;

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

                    if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                        const LogMeta& meta = local_log_metas[header->log_id - 1];
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

                        if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                            const LogMeta& meta = local_log_metas[header->log_id - 1];
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
                        active_buffer_count_--;
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
            std::vector<LogMeta> local_log_metas;
            local_log_metas.reserve(1000);
            IOBuffer io_buf(appender_.get(), config_.io_buffer_size);

            while (log_thread_running_.load(std::memory_order_relaxed)) {
                size_t process_count = consume_buffers_round_robin(local_log_metas, io_buf, false);

                if (process_count < 1) {  // idle
                    io_buf.flush_to_os();
                    TscClock::instance().sync_system_time();

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
            global_log_metas_.reserve(1000);
            thread_buffers_.reserve(100);
            thread_buffers_bg_.reserve(100);
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

        // --- 活跃/非活跃列表分离优化专用 ---
        size_t active_buffer_count_{ 0 }; // 活跃线程分界线 (索引)
        uint64_t poll_cycles_{ 0 };       // 后台空转轮询次数计数器

        std::unique_ptr<ILogAppender> appender_;
        std::thread log_thread_;
        std::atomic<bool> log_thread_running_{ false };

        std::mutex              idle_wait_mutex_;
        std::condition_variable idle_wait_condition_;
        // 强制独占一个缓存行，不和 mutex 沾边
        alignas(CACHE_LINE_SIZE) std::atomic<bool> idle_wait_flag_{ false };

    public:
        //statistics
        std::atomic<uint64_t>  stat_produce_count_{ 0 };
        std::atomic<uint64_t>  stat_produce_valid_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_valid_count_{ 0 };
    };

    ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer* NanoLogger::thread_buffer_ = nullptr;
    thread_local NanoLogger::ThreadBufferDestroyer NanoLogger::thread_buffer_destroyer_;
}

// ---------------------------------------------------------------------------
// 宏定义更新：注入局部的 FmtProvider 结构体以捕获编译期字面量
// ---------------------------------------------------------------------------

#define ZRLOG_INIT_CONF(config) zrlog::NanoLogger::instance().init(config)
#define ZRLOG_INIT(filename, level) zrlog::NanoLogger::instance().init(filename, level)
#define ZRLOG_FINI() zrlog::NanoLogger::instance().fini()

#define ZRLOG_BODY(level, format_str, ...)                                                          \
    do {                                                                                            \
        zrlog::NanoLogger &logger = zrlog::NanoLogger::instance();                                  \
        if (logger.check_level(level)) {                                                            \
            static std::atomic<uint32_t> log_id{0};                                                 \
            struct FmtProvider {                                                                    \
                /* 利用预处理器将前缀和用户的 format_str 完美拼接！*/                               \
                static constexpr auto compile() {                                                   \
                    return FMT_COMPILE("{}.{:09d} {} {} {}:{} " format_str);                        \
                }                                                                                   \
            };                                                                                      \
            logger.log<FmtProvider>(log_id, level, __FILE__, __LINE__, __func__, ##__VA_ARGS__);    \
        }                                                                                           \
    } while (0)

#define ZRLOG_TRACE(format, ...) ZRLOG_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DEBUG(format, ...) ZRLOG_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_INFO(format, ...)  ZRLOG_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_WARN(format, ...)  ZRLOG_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_ERROR(format, ...) ZRLOG_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_FATAL(format, ...) ZRLOG_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)
