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
            char *p = buf + 8;
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

    enum class AppenderType {
        File,
        Console
    };

    struct Config {
        AppenderType appender = AppenderType::File;
        std::string filename;
        LogLevel level = LogLevel::DEBUG;
        uint32_t io_buffer_size = 1024 * 256;
        uint32_t thread_buffer_size = 1024 * 1024 * 4;
        uint32_t per_thread_quota = 256;
        uint32_t idle_wait_interval_us = 500;
    };

    class ILogAppender {
    public:
        virtual ~ILogAppender() = default;
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
            return fwrite(data, 1, len, stdout);
        }

        int flush() override {
            return fflush(stdout);
        }
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
            IOBuffer(ILogAppender* appender, uint32_t size)
                : appender_(appender), size_(size) {
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
                    if (appender_) {
                        appender_->write(src, len);
                    }
                    return;
                }
                std::memcpy(data_.data() + pos_, src, len);
                pos_ += len;
            }

            void flush_to_os() {
                if (pos_ > 0 && appender_) {
                    appender_->write(data_.data(), pos_);
                    pos_ = 0;
                }
            }

            void flush_force() {
                flush_to_os();
                if (appender_) {
                    appender_->flush();
                }
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

            if (!log_thread_running_) {
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

                log_thread_running_ = true;
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
            if (log_thread_running_) {
                log_thread_running_ = false;
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

            ThreadBuffer *buffer = get_thread_buffer();
            if (nullptr != buffer) {
                uint32_t args_size = calculate_args_size(args...);
                uint32_t total_size = sizeof(LogEntryHeader) + args_size;
                char* ptr = buffer->alloc(total_size);
                if (nullptr != ptr) {
                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(ptr);
                    header->total_size = total_size;
                    header->log_id = log_id;
                    header->time = TscClock::now_ns_i();
                    header->thread_id = get_thread_id();
                    char* data_ptr = ptr + sizeof(LogEntryHeader);
                    serialize_args(data_ptr, std::forward<Args>(args)...);
                    buffer->commit(total_size);

                    if (idle_wait_flag_.load(std::memory_order_relaxed)) {
                        idle_wait_flag_.store(false, std::memory_order_relaxed);
                        idle_wait_condition_.notify_one();
                    }

                    return true;
                }
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
            std::memcpy(ptr + sizeof(uint32_t), val.data(), len - 1);
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
            out << "\n";
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
            ThreadBuffer(uint32_t size) : size_(normalize_size(size)) {
                mask_ = size_ - 1;
                buffer_.resize(size_);
            }

            ~ThreadBuffer() = default;

            char* alloc(uint32_t len) {
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = local_write_index_;

                uint32_t free_space = calculate_free_space(read, write);
                if (len > free_space) {
                    return nullptr;
                }

                uint32_t tail_free;
                if (write >= read) {
                    tail_free = size_ - write;
                }
                else {
                    tail_free = read - write - 1;
                }

                constexpr uint32_t LOGENTRY_HEAD_SIZE = static_cast<uint32_t>(sizeof(LogEntryHeader));
                if (len <= tail_free) {
                    uint32_t leftover = tail_free - len;
                    if (leftover >= LOGENTRY_HEAD_SIZE) {
                        return &buffer_[write];
                    }

                    //若剩余空间不够放 header, 放弃在尾部写入，则尝试写 padding(前提 tail_free >= header_sz)
                }

                //尾部不够(或leftover < header_sz), 则尝试写padding(必须能写完整 header)
                if (tail_free >= LOGENTRY_HEAD_SIZE) {
                    write_padding_local(write, tail_free);
                    local_write_index_ = 0;
                    uint32_t tail_end = (write + tail_free) & mask_;
                    write_index_.store(tail_end, std::memory_order_release);
                    return alloc_from_start(len, read);
                }

                return nullptr;
            }

            inline void commit(uint32_t len) {
                uint32_t new_local = (local_write_index_ + len) & mask_;
                local_write_index_ = new_local;
                write_index_.store(new_local, std::memory_order_release);
            }

            bool try_read(char*& out_ptr) {
                uint32_t read  = read_index_.load(std::memory_order_relaxed);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                if (read == write) {
                    return false;
                }

                // 检查剩余空间是否足够读出一个 Header
                // 注意：SPSC中，write 可能 wrap 到了 0，而 read 在末尾
                // 此时 write < read。
                uint32_t space_to_end = size_ - read;
                if (space_to_end < sizeof(LogEntryHeader)) {
                    read_index_.store(0, std::memory_order_release);
                    return false;
                }

                uint32_t read_index  = read_index_.load(std::memory_order_relaxed);
                uint32_t write_index = write_index_.load(std::memory_order_acquire);

                // 缓冲区为空
                if (read_index == write_index) {
                    return false;
                }

                // 循环处理可能遇到的padding
                constexpr int MAX_PADDING_SKIPS = 3; // 最大跳过padding次数
                int skips = 0;
                while (skips < MAX_PADDING_SKIPS) {
                    LogEntryHeader *header = reinterpret_cast<LogEntryHeader*>(&buffer_[read_index]);
                    uint32_t claimed = header->total_size;
                    if (claimed < sizeof(LogEntryHeader) || claimed > size_) {
                        read_index_.store(write, std::memory_order_release);
                        return false;
                    }

                    // 如果遇到padding，跳过它
                    if (header->log_id == PADDING_ID) {
                        // 跳过padding，更新读指针
                        uint32_t new_read_index = (read_index + claimed) & mask_;
                        read_index_.store(new_read_index, std::memory_order_release);

                        // 重新加载指针
                        read_index = new_read_index;
                        skips++;

                        // 检查是否为空
                        if (read_index == write_index) {
                            return false;
                        }
                        continue;
                    }

                    // 有效数据
                    out_ptr = &buffer_[read_index];
                    return true;
                }

                return false;
            }

            inline void consume(uint32_t len) {
                uint32_t read = read_index_.load(std::memory_order_relaxed);
                uint32_t new_read = (read + len) & mask_;
                read_index_.store(new_read, std::memory_order_release);
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

            inline uint32_t estimate_free_space() const {
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                return calculate_free_space(read, write);
            }

            inline uint32_t estimate_used_space() const {
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                return calculate_used_space(read, write);
            }

        private:
            static const uint32_t PADDING_ID = 0xFFFFFFFF;

            static uint32_t normalize_size(uint32_t n) {
                constexpr uint32_t MIN_SIZE = 1024;
                constexpr uint32_t MAX_POW2 = 1u << 30;
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

            inline uint32_t calculate_free_space(uint32_t read, uint32_t write) const {
                if (write >= read) {
                    return (size_ - (write - read) - 1);
                }
                else {
                    return (read - write - 1);
                }
            }

            inline uint32_t calculate_used_space(uint32_t read, uint32_t write) const {
                if (write >= read) {
                    return write - read;
                }
                return size_ - read + write;
            }

            inline void write_padding_local(uint32_t pos, uint32_t pad_size) {
                if (pad_size < sizeof(LogEntryHeader) || pad_size >= size_) {
                    return;
                }
                if (pos + sizeof(LogEntryHeader) > size_) {
                    return;
                }

                LogEntryHeader* padding = reinterpret_cast<LogEntryHeader*>(&buffer_[pos]);
                padding->log_id = PADDING_ID;
                padding->total_size = pad_size;
                padding->time = 0;
                padding->thread_id = 0;
            }

            inline char* alloc_from_start(uint32_t len, uint32_t read) {
                uint32_t head_free = (read == 0) ? (size_ - 1) : (read - 1);
                if (len <= head_free) {
                    return &buffer_[0];
                }
                return nullptr;
            }

            alignas(64) std::atomic<uint32_t> write_index_{ 0 };
            alignas(64) std::atomic<uint32_t> read_index_{ 0 };
            uint32_t local_write_index_ = 0;

            uint32_t size_;
            uint32_t mask_;
            std::vector<char> buffer_;
            bool should_deallocate_ = false;
        };

        class ThreadBufferDestroyer {
        public:
            ~ThreadBufferDestroyer() {
                if (NanoLogger::thread_buffer_) {
                    NanoLogger::thread_buffer_->mark_deallocate();
                }
            }
        };

        // ========================================================================
        // Management
        // ========================================================================
        template<typename... Args>
        uint32_t register_log_meta(std::atomic<uint32_t>& log_id_atom, LogLevel level, const char* file, uint32_t line, const char* func, const char* format) {
            uint32_t id = log_id_atom.load(std::memory_order_acquire);
            if (id != 0) {
                return id;
            }

            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
            id = log_id_atom.load(std::memory_order_relaxed);
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

            log_id_atom.store(new_id, std::memory_order_release);
            return new_id;
        }

        ThreadBuffer* get_thread_buffer() {
            if (nullptr == thread_buffer_) {
                thread_buffer_ = new ThreadBuffer(config_.thread_buffer_size);
                (void)thread_buffer_destroyer_;
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                thread_buffers_.push_back(thread_buffer_);
            }
            return thread_buffer_;
        }

        bool consume_buffers_round_robin(std::vector<LogMeta>& local_log_metas, IOBuffer& io_buf) {
            bool any_data_processed = false;

            {
                std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                if (global_log_metas_.size() > local_log_metas.size()) {
                    local_log_metas.insert(local_log_metas.end(),
                        global_log_metas_.begin() + local_log_metas.size(),
                        global_log_metas_.end());
                }
            }
            {
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                if (!thread_buffers_.empty()) {
                    thread_buffers_bg_.splice(thread_buffers_bg_.end(), thread_buffers_);
                }
            }

            auto it = thread_buffers_bg_.begin();
            while (it != thread_buffers_bg_.end()) {
                ThreadBuffer* tb = *it;
                char* ptr = nullptr;
                bool thread_active = false;
                uint32_t quota = config_.per_thread_quota;

                while (quota > 0 && tb->try_read(ptr)) {
                    any_data_processed = true;
                    thread_active = true;
                    quota--;

                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(ptr);
                    if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                        const LogMeta& meta = local_log_metas[header->log_id - 1];
                        char* args_ptr = ptr + sizeof(LogEntryHeader);
                        meta.decoder(args_ptr, meta, *header, io_buf);
                    }
                    tb->consume(header->total_size);
                }

                if (!thread_active && tb->should_deallocate()) {
                    delete tb;
                    it = thread_buffers_bg_.erase(it);
                }
                else {
                    ++it;
                }
            }
            return any_data_processed;
        }

        void poll_routine() {
            std::vector<LogMeta> local_log_metas;
            local_log_metas.reserve(1000);
            IOBuffer io_buf(appender_.get(), config_.io_buffer_size);

            while (log_thread_running_) {
                bool busy = consume_buffers_round_robin(local_log_metas, io_buf);
                if (!busy) {
                    io_buf.flush_to_os();

                    idle_wait_flag_.store(true, std::memory_order_relaxed);
                    {
                        std::unique_lock<std::mutex> lock(idle_wait_mutex_);
                        idle_wait_condition_.wait_for(lock, std::chrono::microseconds(config_.idle_wait_interval_us));
                    }
                    idle_wait_flag_.store(false, std::memory_order_relaxed);
                }
            }

            while (consume_buffers_round_robin(local_log_metas, io_buf)) {
            }
            io_buf.flush_force();
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
        volatile bool log_thread_running_ = false;

        std::mutex              idle_wait_mutex_;
        std::condition_variable idle_wait_condition_;
        std::atomic_bool        idle_wait_flag_ = false;
    };

    ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer* NanoLogger::thread_buffer_ = nullptr;
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
