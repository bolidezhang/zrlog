#pragma once

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

    // ---------------------------------------------------------------------------
    // 内部细节
    // ---------------------------------------------------------------------------
    namespace detail {
        // 优化：解码 string/string_view 时直接返回指向 buffer 内部的 const char*
        // 实现零拷贝 (前端序列化时已保证以 \0 结尾)
        template <typename T>
        auto decode_val(char*& ptr) {
            if constexpr (std::is_same_v<T, const char*> ||
                std::is_same_v<T, std::string> ||
                std::is_same_v<T, std::string_view>) {
                uint32_t len;
                std::memcpy(&len, ptr, sizeof(uint32_t));
                const char* str = ptr + sizeof(uint32_t);
                ptr += sizeof(uint32_t) + len;
                return str; // 直接返回地址
            }
            else {
                T val;
                std::memcpy(&val, ptr, sizeof(T));
                ptr += sizeof(T);
                return val;
            }
        }

        template<typename... Ts>
        struct TupleDeserializer;

        template<typename Head, typename... Tail>
        struct TupleDeserializer<Head, Tail...> {
            static auto apply(char*& ptr) {
                using RawHead = typename std::decay<Head>::type;
                auto head = decode_val<RawHead>(ptr);
                return std::tuple_cat(std::make_tuple(head), TupleDeserializer<Tail...>::apply(ptr));
            }
        };

        template<>
        struct TupleDeserializer<> {
            static std::tuple<> apply(char*&) {
                return std::tuple<>();
            }
        };

        template <std::size_t... Is> struct index_sequence {};
        template <std::size_t N, std::size_t... Is> struct make_index_sequence : make_index_sequence<N - 1, N - 1, Is...> {};
        template <std::size_t... Is> struct make_index_sequence<0, Is...> : index_sequence<Is...> {};

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

        // 快速转换 9 位纳秒 (补零)
        inline void fast_u64_to_9digits(char* buf, uint64_t val) {
            // 从后往前填充
            if (val > 999999999) {
                val = 999999999;
            }
            char* p = buf + 8;
            for (int i = 0; i < 9; ++i) {
                *p-- = (char)('0' + (val % 10));
                val /= 10;
            }
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
        TscClock(TscClock&&) = delete;
        TscClock& operator=(TscClock&&) = delete;

        struct Anchor {
            uint64_t base_ns = 0;
            uint64_t base_tsc = 0;
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
        Discard = 0,          // 策略1：直接丢弃    (默认，保证业务线程绝对不被阻塞，极致低时延)
        Block = 1,          // 策略2：无限重试    (阻塞直到有空间，保证绝对不丢日志，但会导致业务卡顿)
        Retry = 2           // 策略3：有限次重试  (重试一定次数后仍满，则丢弃，兼顾平滑与低时延)
    };

    struct Config {
        AppenderType appender = AppenderType::File;
        std::string filename;
        LogLevel level = LogLevel::DEBUG;
        uint32_t io_buffer_size = 1024 * 256;           //io缓冲大小(也即日志格式化缓冲, 全局唯一)
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
            char *ptr = const_cast<char*>(data);
            size_t nleft = len;
            int ret;

            do {
                ret = write(ptr, nleft);
                if (ret > 0) {
                    nleft -= ret;
                    ptr += ret;
                }
                else if (ret < 0) {
                    if (errno == EINTR) {
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

        //使用标准的文件描述符ID
        static constexpr int STDOUT_FILENO = 1;
    };

    class NanoLogger {
    public:
        class IOBuffer;
        struct LogMeta;
        struct LogEntryHeader;
        using DecoderFn = void(*)(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, IOBuffer& out);

        struct LogMeta {
            uint32_t    id;
            LogLevel    level;
            const char* file;
            uint32_t    line;
            const char* func;
            std::string format;
            DecoderFn   decoder;
        };

        struct LogEntryHeader {
            uint32_t total_size;
            uint32_t log_id;
            uint64_t time;
            uint64_t thread_id;
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
                if (pos_ + len > size_) {
                    flush_to_os();
                }
                if (len > size_) {
                    appender_->writen(src, len);
                    return;
                }
                std::memcpy(data_.data() + pos_, src, len);
                pos_ += len;
            }

            void flush_to_os() {
                if (pos_ > 0) {
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

            IOBuffer& operator<<(char c) {
                if (available_size() < 1) {
                    flush_to_os();
                }
                data_[pos_++] = c;
                return *this;
            }
            IOBuffer& operator<<(const char* str) {
                if (str) {
                    append(str, std::strlen(str));
                }
                return *this;
            }
            IOBuffer& operator<<(const std::string& str) {
                append(str.data(), str.size());
                return *this;
            }
            template <typename T>
            typename std::enable_if<std::is_integral<T>::value, IOBuffer&>::type operator<<(T val) {
                if (available_size() < 32) {
                    flush_to_os();
                }
                auto res = std::to_chars(current_ptr(), data_.data() + size_, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
            }
            IOBuffer& operator<<(float val) {
                if (available_size() < 64) {
                    flush_to_os();
                }
                auto res = std::to_chars(current_ptr(), data_.data() + size_, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
            }
            IOBuffer& operator<<(double val) {
                if (available_size() < 64) {
                    flush_to_os();
                }
                auto res = std::to_chars(current_ptr(), data_.data() + size_, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
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
        // Frontend
        // ========================================================================
        template<typename... Args>
        bool log(std::atomic<uint32_t>& log_id_atom, LogLevel level, const char* file, uint32_t line,
            const char* func, const char* format, Args&&... args) {

            uint32_t log_id = log_id_atom.load(std::memory_order_relaxed);
            if (0 == log_id) {
                log_id = register_log_meta<Args...>(log_id_atom, level, file, line, func, format);
            }

            ThreadBuffer* buffer = get_thread_buffer();
            if (nullptr != buffer) {
                //stat_produce_count_.fetch_add(1, std::memory_order_relaxed);
                uint32_t args_size = calculate_args_size(args...);
                uint32_t total_size = sizeof(LogEntryHeader) + args_size;
                char *ptr = buffer->alloc(total_size);
                if (nullptr == ptr) {
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
                            } while (nullptr == ptr);
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
                            } while (nullptr == ptr);
                        }
                        break;

                    default:
                        return false;

                    }
                }

                // 如果走到这里，说明 ptr 必然不为 nullptr (分配成功)
                LogEntryHeader *header = reinterpret_cast<LogEntryHeader *>(ptr);
                header->total_size = total_size;
                header->log_id = log_id;
                header->time = TscClock::now_ns_i();
                header->thread_id = get_thread_id();
                serialize_args(ptr + sizeof(LogEntryHeader), std::forward<Args>(args)...);
                buffer->commit(total_size);
                //stat_produce_valid_count_.fetch_add(1, std::memory_order_relaxed);
                notify_consumer();

                return true;
            }

            return false;
        }

    private:
        static uint32_t calculate_args_size() {
            return 0;
        }
        template<typename T, typename... Rest>
        static uint32_t calculate_args_size(const T& val, const Rest&... rest) {
            return sizeof(T) + calculate_args_size(rest...);
        }
        static uint32_t calculate_args_size(const char* val) {
            return sizeof(uint32_t) + (val ? (uint32_t)strlen(val) + 1 : 1);
        }
        template<typename... Rest>
        static uint32_t calculate_args_size(const char* val, const Rest&... rest) {
            return calculate_args_size(val) + calculate_args_size(rest...);
        }
        static uint32_t calculate_args_size(const std::string& val) {
            return sizeof(uint32_t) + (uint32_t)val.size() + 1;
        }
        template<typename... Rest>
        static uint32_t calculate_args_size(const std::string& val, const Rest&... rest) {
            return calculate_args_size(val) + calculate_args_size(rest...);
        }
        static uint32_t calculate_args_size(std::string_view val) {
            return sizeof(uint32_t) + (uint32_t)val.size() + 1;
        }
        template<typename... Rest>
        static uint32_t calculate_args_size(std::string_view val, const Rest&... rest) {
            return calculate_args_size(val) + calculate_args_size(rest...);
        }

        static void serialize_args(char* ptr) {
        }
        template<typename T, typename... Rest>
        static void serialize_args(char* ptr, const T& val, Rest&&... rest) {
            std::memcpy(ptr, &val, sizeof(T));
            serialize_args(ptr + sizeof(T), std::forward<Rest>(rest)...);
        }
        template<typename... Rest>
        static void serialize_args(char* ptr, const char* val, Rest&&... rest) {
            uint32_t len = val ? (uint32_t)strlen(val) + 1 : 1;
            std::memcpy(ptr, &len, sizeof(uint32_t));
            if (val) {
                std::memcpy(ptr + sizeof(uint32_t), val, len);
            }
            else {
                *reinterpret_cast<char*>(ptr + sizeof(uint32_t)) = '\0';
            }
           serialize_args(ptr + sizeof(uint32_t) + len, std::forward<Rest>(rest)...);
        }
        template<typename... Rest>
        static void serialize_args(char* ptr, const std::string& val, Rest&&... rest) {
            uint32_t len = (uint32_t)val.size() + 1;
            std::memcpy(ptr, &len, sizeof(uint32_t));
            if (len > 1) {
                std::memcpy(ptr + sizeof(uint32_t), val.data(), len - 1);
            }
            *(ptr + sizeof(uint32_t) + len - 1) = '\0';
            serialize_args(ptr + sizeof(uint32_t) + len, std::forward<Rest>(rest)...);
        }
        template<typename... Rest>
        static void serialize_args(char* ptr, std::string_view val, Rest&&... rest) {
            uint32_t len = (uint32_t)val.size() + 1;
            std::memcpy(ptr, &len, sizeof(uint32_t));
            if (len > 1) {
                std::memcpy(ptr + sizeof(uint32_t), val.data(), len - 1);
            }
            *(ptr + sizeof(uint32_t) + len - 1) = '\0';
            serialize_args(ptr + sizeof(uint32_t) + len, std::forward<Rest>(rest)...);
        }

        // ========================================================================
        // Backend
        // ========================================================================

        template<typename Tuple, std::size_t... Is>
        static void format_log_impl(IOBuffer& out, const char* fmt, const Tuple& t, detail::index_sequence<Is...>) {
            size_t space = out.available_size();
            if (space < 256) {
                out.flush_to_os();
                space = out.available_size();
            }

            char* ptr = out.current_ptr();
            // Tuple 元素中的 string/string_view 已被 decode_val 转换为 const char*
            int len = snprintf(ptr, space, fmt, std::get<Is>(t)...);
            if (len < 0) {
                return;
            }

            if (static_cast<size_t>(len) >= space) {
                out.flush_to_os();
                space = out.available_size();
                ptr = out.current_ptr();
                int retry_len = snprintf(ptr, space, fmt, std::get<Is>(t)...);
                if (retry_len > 0) {
                    size_t written = (static_cast<size_t>(retry_len) < space) ? static_cast<size_t>(retry_len) : (space - 1);
                    out.advance(written);
                }
            }
            else {
                out.advance(len);
            }
            //out << "\n";
            out << '\n';
        }


        static void format_timestamp(IOBuffer& out, uint64_t ns_time) {
            // 缓存秒级字符串: "YYYY-MM-DD HH:MM:SS" (19 chars)
            static thread_local time_t cache_sec = 0;
            static thread_local char cache_str[20] = { 0 }; // 19 + 1(\0)

            time_t sec = static_cast<time_t>(ns_time / 1000000000);
            if (sec != cache_sec) {
                struct tm t;
                zrlog_gmtime(&sec, &t);

                // 手动格式化，移除 snprintf
                // format: YYYY-MM-DD HH:MM:SS
                detail::fast_u32_to_4digits(cache_str, t.tm_year + 1900);
                cache_str[4] = '-';
                detail::fast_u32_to_2digits(cache_str + 5, t.tm_mon + 1);
                cache_str[7] = '-';
                detail::fast_u32_to_2digits(cache_str + 8, t.tm_mday);
                cache_str[10] = ' ';
                detail::fast_u32_to_2digits(cache_str + 11, t.tm_hour);
                cache_str[13] = ':';
                detail::fast_u32_to_2digits(cache_str + 14, t.tm_min);
                cache_str[16] = ':';
                detail::fast_u32_to_2digits(cache_str + 17, t.tm_sec);
                cache_str[19] = '\0'; // 虽然后面是 append(ptr, len) 不依赖 \0，但保留是个好习惯

                cache_sec = sec;
            }
            out.append(cache_str, 19);
            out << '.';

            // 纳秒部分: 9位补零
            uint64_t nano = ns_time % 1000000000;
            char nano_buf[10]; // 9 + 1
            detail::fast_u64_to_9digits(nano_buf, nano);
            out.append(nano_buf, 9);
            out << ' ';
        }

        template<typename... Args>
        static void generated_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, IOBuffer& out) {
            // 1. 解码用户参数 (只包含日志内容参数，不包含头部)
            auto args_tuple = detail::TupleDeserializer<typename std::decay<Args>::type...>::apply(buffer);

            // 2. 格式化时间戳
            format_timestamp(out, header.time);

            // 3. 格式化固定头部 (Level, ThreadID, File:Line)
            out << loglevel_to_string(meta.level) << " "
                << header.thread_id << " "
                << meta.file << ":" << meta.line << " ";

            // 4. 格式化用户日志内容
            format_log_impl(out, meta.format.c_str(), args_tuple,
                detail::make_index_sequence<std::tuple_size<decltype(args_tuple)>::value>{});
        }


        class ThreadBuffer {
        public:
            explicit ThreadBuffer(uint32_t size) : size_(normalize_size(size)) {
                mask_ = size_ - 1;
                buffer_.resize(size_);
            }

            ~ThreadBuffer() = default;

            // 分配内存供写入
            char* alloc(uint32_t len) {
                // 单写者：write_index_ 用 relaxed 即可；需 acquire read_index_ 获取消费者最新进度
                uint64_t w = write_index_.load(std::memory_order_relaxed);
                uint64_t r = read_index_.load(std::memory_order_acquire);

                // 1. 全局防线：检查总逻辑剩余空间是否足够
                if (w + len - r > size_) {
                    return nullptr;
                }

                uint32_t phys_w    = w & mask_;
                uint32_t tail_free = size_ - phys_w;

                // 2. 如果尾部物理空间连续且足够，直接分配
                if (len <= tail_free) {
                    return buffer_.data() + phys_w;
                }

                // 3. 尾部空间不够，需要绕回 (Wrap around)
                // 再次检查：算上为了绕回而产生的 padding 浪费，总空间还够不够？
                if (w + tail_free + len - r > size_) {
                    return nullptr;
                }

                constexpr uint32_t HEADER_SIZE = sizeof(LogEntryHeader);

                // 写入 Padding（如果尾部连一个 Header 都放不下，就不写，依赖消费端的隐式跳过）
                if (tail_free >= HEADER_SIZE) {
                    write_padding_local(phys_w, tail_free);
                }

                // 推进逻辑写下标，补齐到物理 0 处
                w += tail_free;

                // 提前提交 padding 给消费者可见 (Release 语义)
                write_index_.store(w, std::memory_order_release);

                // 绕回后，从头部物理 0 处分配
                return buffer_.data();
            }

            // 提交数据，使其对消费者可见
            inline void commit(uint32_t len) {
                uint64_t w = write_index_.load(std::memory_order_relaxed);
                w += len; // 逻辑下标单调递增
                write_index_.store(w, std::memory_order_release);
            }

            // 尝试读取数据
            LogEntryHeader* try_read() {
                // 单读者：read_index_ 用 relaxed；需 acquire write_index_ 获取生产者最新提交
                uint64_t r = read_index_.load(std::memory_order_relaxed);
                uint64_t w = write_index_.load(std::memory_order_acquire);

                constexpr int MAX_SKIPS = 8;
                int skips = 0;

                while (skips < MAX_SKIPS && r < w) {
                    uint32_t phys_r = r & mask_;
                    uint32_t tail_avail = size_ - phys_r;
                    constexpr uint32_t HEADER_SIZE = sizeof(LogEntryHeader);

                    // 1. 隐式 Padding 处理：如果尾部连 Header 都读不全，说明生产者直接绕回了
                    if (tail_avail < HEADER_SIZE) {
                        r += tail_avail; // 逻辑推进到下一圈的 0
                        read_index_.store(r, std::memory_order_release);
                        continue; // 重新评估 r < w
                    }

                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(buffer_.data() + phys_r);

                    // 2. 显式 Padding 处理
                    if (header->log_id == PADDING_ID) {
                        uint32_t claimed = header->total_size;
                        // 安全校验，防止脏数据死循环
                        if (claimed < HEADER_SIZE || claimed > tail_avail) {
                            return nullptr;
                        }
                        r += claimed;
                        read_index_.store(r, std::memory_order_release);
                        ++skips;
                        continue;
                    }

                    // 3. 拦截未 Commit 的数据 (极其重要)
                    // 虽然生产者写入了 Header，但可能还没调用 commit(len)，
                    // 此时 `w` 还没有包容整个 message 的长度。必须等待 commit 推进 `w`。
                    if (r + header->total_size > w) {
                        return nullptr;
                    }

                    return header;
                }

                return nullptr;
            }

            // 消费完毕，推进读游标
            inline void consume(uint32_t len) {
                uint64_t r = read_index_.load(std::memory_order_relaxed);
                r += len; // 逻辑下标单调递增
                read_index_.store(r, std::memory_order_release);
            }

            inline bool should_deallocate() const { 
                return should_deallocate_; 
            }

            inline void mark_deallocate() { 
                should_deallocate_ = true; 
            }

            inline uint32_t capacity() const { 
                return size_; 
            }

            // O(1) 的空间预估
            inline uint32_t estimate_used_space() const {
                uint64_t w = write_index_.load(std::memory_order_acquire);
                uint64_t r = read_index_.load(std::memory_order_acquire);
                // 防止多线程下 r 和 w 的瞬间错位导致负数
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
                p->thread_id = 0;
            }

            // ----------------- 内存对齐，防止伪共享 (False Sharing) -----------------
            alignas(64) std::atomic<uint64_t> write_index_{ 0 };
            char pad1[64 - sizeof(std::atomic<uint64_t>)];

            alignas(64) std::atomic<uint64_t> read_index_{ 0 };
            char pad2[64 - sizeof(std::atomic<uint64_t>)];

            uint32_t size_;
            uint32_t mask_;
            std::vector<char> buffer_;
            bool should_deallocate_ = false;
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
        template<typename... Args>
        uint32_t register_log_meta(std::atomic<uint32_t>& log_id_atom, LogLevel level, const char* file, uint32_t line, const char* func, const char* format) {
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
            new_log.format = format;
            new_log.decoder = &generated_decoder<Args...>;
            global_log_metas_.push_back(std::move(new_log));

            log_id_atom.store(new_id, std::memory_order_relaxed);
            return new_id;
        }

            ThreadBuffer* get_thread_buffer() {
            if (nullptr == thread_buffer_) {
                thread_buffer_ = new ThreadBuffer(config_.thread_buffer_size);
                thread_buffer_destroyer_.init();
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                thread_buffers_.push_back(thread_buffer_);
            }
            return thread_buffer_;
        }

        size_t consume_buffers_round_robin(std::vector<LogMeta>& local_log_metas, IOBuffer& io_buf, bool full_drain = false) {
            size_t processed_count = 0;

            {
                //追加日志元数据到本地集合
                std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                if (global_log_metas_.size() > local_log_metas.size()) {
                    local_log_metas.insert(local_log_metas.end(),
                        global_log_metas_.begin() + local_log_metas.size(),
                        global_log_metas_.end());
                }
            }
            {
                //追加ThreadBuffer到后台集合thread_buffers_bg_
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                if (!thread_buffers_.empty()) {
                    thread_buffers_bg_.splice(thread_buffers_bg_.end(), thread_buffers_);
                }
            }

            LogEntryHeader *header;
            auto it = thread_buffers_bg_.begin();
            while (it != thread_buffers_bg_.end()) {
                ThreadBuffer *tb = *it;
                uint32_t quota = full_drain ? UINT32_MAX : config_.per_thread_quota;
                while ((quota > 0) && (header = tb->try_read())) {
                    stat_consume_count_.fetch_add(1, std::memory_order_relaxed);
                    ++processed_count;
                    --quota;

                    if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                        const LogMeta& meta = local_log_metas[header->log_id - 1];
                        char *args_ptr = (char *)header + sizeof(LogEntryHeader);
                        meta.decoder(args_ptr, meta, *header, io_buf);
                        stat_consume_valid_count_.fetch_add(1, std::memory_order_relaxed);
                    } 
                    else {  //header->log_id 无效时
                        {
                            //追加日志元数据到本地集合
                            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                            if (global_log_metas_.size() > local_log_metas.size()) {
                                local_log_metas.insert(local_log_metas.end(),
                                    global_log_metas_.begin() + local_log_metas.size(),
                                    global_log_metas_.end());
                            }
                        }

                        if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                            const LogMeta& meta = local_log_metas[header->log_id - 1];
                            char *args_ptr = (char *)header + sizeof(LogEntryHeader);
                            meta.decoder(args_ptr, meta, *header, io_buf);
                            stat_consume_valid_count_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }

                    tb->consume(header->total_size);
                }

                if (tb->should_deallocate() && tb->estimate_used_space() == 0) {
                    delete tb;
                    it = thread_buffers_bg_.erase(it);
                }
                else {
                    ++it;
                }
            }

            return processed_count;
        }

        void poll_routine() {
            std::vector<LogMeta> local_log_metas;
            local_log_metas.reserve(1000);
            IOBuffer io_buf(appender_.get(), config_.io_buffer_size);

            while (log_thread_running_.load(std::memory_order_relaxed)) {
                uint32_t process_count = consume_buffers_round_robin(local_log_metas, io_buf, false);
                if (process_count < 1) {  //idle
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
        std::list<ThreadBuffer*> thread_buffers_;
        std::list<ThreadBuffer*> thread_buffers_bg_;

        std::unique_ptr<ILogAppender> appender_;
        std::thread log_thread_;
        std::atomic<bool> log_thread_running_ = false;

        std::mutex              idle_wait_mutex_;
        std::condition_variable idle_wait_condition_;
        std::atomic<bool>       idle_wait_flag_ = false;

    public:
        //statistics
        std::atomic<uint64_t>  stat_produce_count_{ 0 };
        std::atomic<uint64_t>  stat_produce_valid_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_count_{ 0 };
        std::atomic<uint64_t>  stat_consume_valid_count_{ 0 };
    };

    ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer *NanoLogger::thread_buffer_ = nullptr;
    thread_local NanoLogger::ThreadBufferDestroyer NanoLogger::thread_buffer_destroyer_;
}

// ---------------------------------------------------------------------------
// 宏定义更新
// ---------------------------------------------------------------------------

#define ZRLOG_INIT_CONF(config) zrlog::NanoLogger::instance().init(config)
#define ZRLOG_INIT(filename, level) zrlog::NanoLogger::instance().init(filename, level)
#define ZRLOG_FINI() zrlog::NanoLogger::instance().fini()

#define ZRLOG_BODY(level, format, ...)                                                      \
    do {                                                                                    \
        zrlog::NanoLogger &logger = zrlog::NanoLogger::instance();                          \
        if (logger.check_level(level)) {                                                    \
            static std::atomic<uint32_t> log_id{0};                                         \
            logger.log(log_id, level, __FILE__, __LINE__, __func__, format, ##__VA_ARGS__); \
        }                                                                                   \
    } while (0)

#define ZRLOG_TRACE(format, ...) ZRLOG_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DEBUG(format, ...) ZRLOG_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_INFO(format, ...)  ZRLOG_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_WARN(format, ...)  ZRLOG_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_ERROR(format, ...) ZRLOG_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_FATAL(format, ...) ZRLOG_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)
