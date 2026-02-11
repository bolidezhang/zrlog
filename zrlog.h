#pragma once

#include <vector>
#include <list>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
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
#include <memory> // std::unique_ptr

// ---------------------------------------------------------------------------
// 平台差异处理 & 内部宏定义
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <share.h>
#include <intrin.h>

#define ZRLOG_OPEN(path, flags, mode)   _sopen(path, flags, _SH_DENYNO, mode)
#define ZRLOG_WRITE(fd, data, len)      _write(fd, data, (unsigned int)len)
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
        template <typename T>
        typename std::enable_if<!std::is_same<T, const char*>::value, T>::type
            decode_val(char*& ptr) {
            T val;
            std::memcpy(&val, ptr, sizeof(T));
            ptr += sizeof(T);
            return val;
        }

        template <typename T>
        typename std::enable_if<std::is_same<T, const char*>::value, const char*>::type
            decode_val(char*& ptr) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            const char* str = ptr + sizeof(uint32_t);
            ptr += sizeof(uint32_t) + len;
            return str;
        }

        template<typename... Ts>
        struct TupleDeserializer;

        template<typename Head, typename... Tail>
        struct TupleDeserializer<Head, Tail...> {
            static std::tuple<Head, Tail...> apply(char*& ptr) {
                using CleanHead = typename std::decay<Head>::type;
                CleanHead head = decode_val<CleanHead>(ptr);
                return std::tuple_cat(std::tuple<CleanHead>(head), TupleDeserializer<Tail...>::apply(ptr));
            }
        };

        template<>
        struct TupleDeserializer<> {
            static std::tuple<> apply(char*&) {
                return std::tuple<>();
            }
        };

        template <std::size_t... Is> struct index_sequence {
        };

        template <std::size_t N, std::size_t... Is> struct make_index_sequence : make_index_sequence<N - 1, N - 1, Is...> {
        };

        template <std::size_t... Is> struct make_index_sequence<0, Is...> : index_sequence<Is...> {
        };
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

    //基于OS的tsc计时器的时钟
    class TscClock {
    public:
        TscClock(const TscClock&) = delete;
        TscClock& operator=(const TscClock&) = delete;
        TscClock(TscClock&&) = delete;
        TscClock& operator=(TscClock&&) = delete;

        struct Anchor {
            uint64_t base_ns = 0;  //系统时间基准
            uint64_t base_tsc = 0;  //TSC基准值
        };

        static TscClock& instance() {
            static TscClock t;
            return t;
        }

        // 当前rdtsc
        static inline uint64_t rdtsc() {
            return zrlog_rdtsc();
        }

        // 获取当前时间（纳秒）
        static inline uint64_t now_ns() {
            return instance().current_time_ns();
        }

        // 核心函数：获取高精度时间
        inline uint64_t current_time_ns() const {
            uint64_t tsc = zrlog_rdtsc();
            uint32_t seq;
            Anchor anc;

            // SeqLock 读操作：无锁，通过版本号重试保证一致性
            do {
                seq = seq_.load(std::memory_order_acquire);
                anc = anchor_;
                std::atomic_thread_fence(std::memory_order_acquire); // 保证读取 anchor 在读取 seq 之后
            } while (seq != seq_.load(std::memory_order_relaxed) || (seq & 1));

            return anc.base_ns + tsc2ns(tsc - anc.base_tsc);
        }

        // 将 TSC 差值转换为纳秒
        inline uint64_t tsc2ns(uint64_t tsc_diff) const {
#if defined(_MSC_VER) && defined(_M_X64)
            // MSVC x64 优化路径
            unsigned __int64 high;
            unsigned __int64 low = _umul128(tsc_diff, multiplier_, &high);
            return (high << (64 - shift_)) | (low >> shift_);
#elif defined(__SIZEOF_INT128__)
            // GCC/Clang 128位整数路径
            return (uint64_t)((unsigned __int128)tsc_diff * multiplier_ >> shift_);
#else
            // 32位系统或旧编译器回退路径：使用浮点数（会有性能损失，但在32位机上通常可接受）
            // 或者拆分乘法。这里为了代码简洁使用 double，如果需要极致性能需手写32位拆分乘法
            return static_cast<uint64_t>(tsc_diff * multiplier_d_);
#endif
        }

        // 重新校准（通常不需要频繁调用）
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

            // 取中位数
            std::sort(rates.begin(), rates.end());
            double rate = rates[rates.size() / 2];

            // 写入保护
            uint32_t seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_release); // 变为奇数，阻塞读者

            multiplier_ = static_cast<uint64_t>(rate * (1ULL << shift_));
#if !defined(_MSC_VER) || !defined(_M_X64)
#if !defined(__SIZEOF_INT128__)
            multiplier_d_ = rate; // 32位环境备用
#endif
#endif

        // 更新基准点
            sync_anchor_unlocked();
            seq_.store(seq + 2, std::memory_order_release); // 变为偶数，释放读者

            return true;
        }

        // 同步系统时间锚点
        void sync_system_time() {
            uint32_t seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_release);
            sync_anchor_unlocked();
            seq_.store(seq + 2, std::memory_order_release);
        }

    private:
        TscClock() {
            if (!calibrate()) {
                // 如果校准彻底失败，设置一个保底值（假设 1GHz）
                multiplier_ = 1ULL << shift_;
            }
        }

        void sync_anchor_unlocked() {
            // 获取高精度的系统时间作为基准
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

        // 使用 SeqLock 替代 atomic<Anchor>
        // seq_ 为偶数时表示数据稳定，奇数时表示正在写入
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

    // ---------------------------------------------------------------------------
    // 配置结构体
    // ---------------------------------------------------------------------------
    enum class AppenderType {
        File,
        Console
    };

    struct Config {
        AppenderType appender = AppenderType::File;
        std::string filename;
        LogLevel level = LogLevel::DEBUG;
        uint32_t io_buffer_size     = 1024 * 256;
        uint32_t thread_buffer_size = 1024 * 1024 * 4;
        uint32_t per_thread_quota   = 256;
    };

    // ---------------------------------------------------------------------------
    // LogAppender 接口体系
    // ---------------------------------------------------------------------------
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
        static constexpr const char* HEAD_FORMATS = "%04d-%02d-%02d %02d:%02d:%02d.%09llu %s %llu %s:%u ";

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
            std::string full_format;
            DecoderFn   decoder;
        };

        struct LogEntryHeader {
            uint32_t total_size;
            uint32_t log_id;
            uint64_t time;
            uint64_t thread_id;
        };

        // -----------------------------------------------------------------------
        // IOBuffer (使用 ILogAppender)
        // -----------------------------------------------------------------------
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
                    if (appender_) appender_->write(src, len);
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
                if (appender_) appender_->flush();
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

            // Stream Operators
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
            std::vector<char> data_;
            size_t size_;
            size_t pos_ = 0;
            ILogAppender *appender_ = nullptr;
        };

        static NanoLogger& instance() {
            static NanoLogger logger;
            return logger;
        }

        // -----------------------------------------------------------------------
        // 初始化
        // -----------------------------------------------------------------------
        bool init(const Config& config) {
            config_ = config;

            if (!log_thread_running_) {
                // 根据配置创建 Appender
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

        // 兼容旧接口
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
                    log_thread_.join();
                }
                appender_.reset(); // 关闭文件
            }
        }

        inline bool check_level(LogLevel level) const {
            return level >= config_.level;
        }

        // ========================================================================
        // Frontend
        // ========================================================================
        template<typename... Args>
        bool log(uint32_t& log_id, LogLevel level, const char* file, uint32_t line,
            const char* func, const char* format, Args&&... args) {

            if (log_id == 0) {
                register_log_meta<Args...>(log_id, level, file, line, func, format);
            }

            ThreadBuffer* buffer = get_thread_buffer();
            if (!buffer) {
                return false;
            }

            uint32_t args_size = calculate_args_size(args...);
            uint32_t total_size = sizeof(LogEntryHeader) + args_size;
            char *ptr = buffer->alloc(total_size);
            if (ptr) {
                LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(ptr);
                header->total_size  = total_size;
                header->log_id      = log_id;
                header->time        = TscClock::now_ns();
                header->thread_id   = get_thread_id();

                char* data_ptr = ptr + sizeof(LogEntryHeader);
                serialize_args(data_ptr, std::forward<Args>(args)...);

                buffer->commit(total_size);
            }
            return true;
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
        static void serialize_args(char* ptr) {}
        template<typename T, typename... Rest>
        static void serialize_args(char* ptr, const T& val, Rest&&... rest) {
            std::memcpy(ptr, &val, sizeof(T)); serialize_args(ptr + sizeof(T), std::forward<Rest>(rest)...);
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

            char *ptr = out.current_ptr();
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

        template<typename... Args>
        static void generated_decoder(char*& buffer, const LogMeta& meta, const LogEntryHeader& header, IOBuffer& out) {
            auto args_tuple = detail::TupleDeserializer<typename std::decay<Args>::type...>::apply(buffer);
            time_t seconds = static_cast<time_t>(header.time / 1000000000);
            uint64_t nanoseconds = header.time % 1000000000;
            struct tm t;
            zrlog_gmtime(&seconds, &t);

            auto head_tuple = std::make_tuple(
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec,
                (unsigned long long)nanoseconds,
                loglevel_to_string(meta.level),
                (uint64_t)header.thread_id,
                meta.file,
                meta.line
            );
            auto full_tuple = std::tuple_cat(head_tuple, args_tuple);
            format_log_impl(out, meta.full_format.c_str(), full_tuple,
                detail::make_index_sequence<std::tuple_size<decltype(full_tuple)>::value>{});
        }

        class ThreadBuffer {
        public:
            ThreadBuffer(uint32_t size) : size_(size) {
                // 确保大小是2的幂，便于环绕计算
                if ((size_ & (size_ - 1)) != 0) {
                    // 找到下一个2的幂
                    size_--;
                    size_ |= size_ >> 1;
                    size_ |= size_ >> 2;
                    size_ |= size_ >> 4;
                    size_ |= size_ >> 8;
                    size_ |= size_ >> 16;
                    size_++;
                }
                mask_ = size_ - 1;
                buffer_.resize(size_);
            }

            ~ThreadBuffer() = default;

            // =======================================================================
            // alloc() - 分配空间（非递归）
            // =======================================================================
            char* alloc(uint32_t len) {
                // 需要总空间：日志头 + 数据 + 1字节（区分满/空状态）
                uint32_t required = len + 1;

                // 预加载读写索引
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = write_index_.load(std::memory_order_relaxed);

                // 计算可用空间
                uint32_t free_space = calculate_free_space(read, write);

                // 检查是否有足够空间
                if (required > free_space) {
                    return nullptr;
                }

                // 检查尾部是否有足够的连续空间
                uint32_t available_till_end = size_ - write;

                if (len <= available_till_end) {
                    // 尾部有足够连续空间
                    // 检查是否需要写入padding（如果尾部空间不足以放下下一个LogEntryHeader）
                    if (available_till_end - len < sizeof(LogEntryHeader) + 1) {
                        // 尾部剩余空间太小，需要写入padding
                        if (available_till_end >= sizeof(LogEntryHeader)) {
                            write_padding(write, available_till_end);
                        }
                        write_index_.store(0, std::memory_order_release);
                        return alloc_from_start(len);
                    }
                    return &buffer_[write];
                }
                else {
                    // 需要环绕
                    // 在尾部写入padding（如果空间足够）
                    if (available_till_end >= sizeof(LogEntryHeader)) {
                        write_padding(write, available_till_end);
                    }

                    // 更新写指针到头部
                    write_index_.store(0, std::memory_order_release);

                    // 从头部开始分配
                    return alloc_from_start(len);
                }
            }

            // =======================================================================
            // commit() - 提交已分配的空间
            // =======================================================================
            void commit(uint32_t len) {
                uint32_t write = write_index_.load(std::memory_order_relaxed);
                uint32_t new_write = (write + len) & mask_;
                write_index_.store(new_write, std::memory_order_release);
            }

            // =======================================================================
            // try_read() - 尝试读取数据（非递归）
            // =======================================================================
            bool try_read(char*& out_ptr) {
                uint32_t read  = read_index_.load(std::memory_order_relaxed);
                uint32_t write = write_index_.load(std::memory_order_acquire);

                // 缓冲区为空
                if (read == write) {
                    return false;
                }

                // 循环处理可能遇到的padding
                constexpr int MAX_PADDING_SKIPS = 3; // 最大跳过padding次数
                int skips = 0;
                while (skips < MAX_PADDING_SKIPS) {
                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(&buffer_[read]);

                    // 如果遇到padding，跳过它
                    if (header->log_id == PADDING_ID) {
                        // 跳过padding，更新读指针
                        uint32_t padding_size = header->total_size;
                        uint32_t new_read = (read + padding_size) & mask_;
                        read_index_.store(new_read, std::memory_order_release);

                        // 重新加载指针
                        read = new_read;
                        skips++;

                        // 检查是否为空
                        if (read == write) {
                            return false;
                        }
                        continue;
                    }

                    // 有效数据
                    out_ptr = &buffer_[read];
                    return true;
                }

                return false;
            }

            // =======================================================================
            // consume() - 消费已读取的数据
            // =======================================================================
            void consume(uint32_t len) {
                uint32_t read     = read_index_.load(std::memory_order_relaxed);
                uint32_t new_read = (read + len) & mask_;
                read_index_.store(new_read, std::memory_order_release);
            }

            // =======================================================================
            // 状态查询
            // =======================================================================
            bool should_deallocate() const {
                return should_deallocate_;
            }

            void mark_deallocate() {
                should_deallocate_ = true;
            }

            uint32_t capacity() const {
                return size_;
            }

            // 估算可用空间
            uint32_t estimate_free_space() const {
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                return calculate_free_space(read, write);
            }

            // 估算已用空间
            uint32_t estimate_used_space() const {
                uint32_t read  = read_index_.load(std::memory_order_acquire);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                return calculate_used_space(read, write);
            }

        private:
            static const uint32_t PADDING_ID = 0xFFFFFFFF;

            // =======================================================================
            // 内部辅助方法
            // =======================================================================

            // 计算可用空间
            uint32_t calculate_free_space(uint32_t read, uint32_t write) const {
                if (write >= read) {
                    // |------已读------|------未读------|------空闲------|
                    // 0               read            write           size
                    return (size_ - write) + read - 1; // -1 用于区分满/空状态
                }
                else {
                    // |------未读------|------空闲------|------已读------|
                    // 0               write           read            size
                    return read - write - 1;
                }
            }

            // 计算已用空间
            uint32_t calculate_used_space(uint32_t read, uint32_t write) const {
                if (write >= read) {
                    return write - read;
                }
                else {
                    return size_ - read + write;
                }
            }

            // 写入padding
            void write_padding(uint32_t pos, uint32_t size) {
                if (pos + sizeof(LogEntryHeader) > size_) {
                    return; // 位置无效
                }

                LogEntryHeader *padding = reinterpret_cast<LogEntryHeader*>(&buffer_[pos]);
                padding->log_id = PADDING_ID;
                padding->total_size = size;
                padding->time = 0;
                padding->thread_id = 0;
            }

            // 从头部开始分配空间
            char* alloc_from_start(uint32_t len) {
                uint32_t write = write_index_.load(std::memory_order_relaxed);
                uint32_t read  = read_index_.load(std::memory_order_acquire);

                // 检查头部空间是否足够
                if (write >= read) {
                    // 写指针在读指针之后，头部空间就是read-1
                    if (len < read - 1) { // -1 确保不覆盖读指针
                        return &buffer_[0];
                    }
                }
                else {
                    // 写指针在读指针之前，头部空间是read-write-1
                    if (len < read - write - 1) {
                        return &buffer_[write];
                    }
                }

                return nullptr;
            }
            
            // =======================================================================
            // 成员变量
            // =======================================================================
            uint32_t size_;                    // 缓冲区大小（2的幂）
            uint32_t mask_;                    // 掩码，用于环绕计算
            std::vector<char> buffer_;         // 缓冲区数据

            alignas(64) std::atomic<uint32_t> write_index_{ 0 };  // 写指针
            alignas(64) std::atomic<uint32_t> read_index_{ 0 };   // 读指针

            bool should_deallocate_ = false;   // 标记是否需要释放
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
        void register_log_meta(uint32_t &log_id, LogLevel level, const char* file, uint32_t line, const char* func, const char* format) {
            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
            if (log_id == 0) {
                log_id = static_cast<uint32_t>(global_log_metas_.size() + 1);
                LogMeta new_log;
                new_log.id = log_id;
                new_log.level = level;
                new_log.file = file;
                new_log.line = line;
                new_log.func = func;
                new_log.format = format;
                new_log.full_format.reserve(256);
                new_log.full_format.append(HEAD_FORMATS);
                new_log.full_format.append(format);
                new_log.decoder = &generated_decoder<Args...>;
                global_log_metas_.push_back(std::move(new_log));
            }
        }

        ThreadBuffer* get_thread_buffer() {
            if (thread_buffer_ == nullptr) {
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

                    LogEntryHeader *header = reinterpret_cast<LogEntryHeader*>(ptr);
                    if (header->log_id > 0 && header->log_id <= local_log_metas.size()) {
                        const LogMeta& meta = local_log_metas[header->log_id - 1];
                        char *args_ptr = ptr + sizeof(LogEntryHeader);
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
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }

            while (consume_buffers_round_robin(local_log_metas, io_buf)) {
                // Drain loop
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

        // 使用 unique_ptr 管理生命周期
        std::unique_ptr<ILogAppender> appender_;
        std::thread log_thread_;

        volatile bool log_thread_running_ = false;
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

#define ZRLOG_BODY(level, format, ...) \
    do { \
        if (zrlog::NanoLogger::instance().check_level(level)) { \
            static uint32_t log_id = 0; \
            zrlog::NanoLogger::instance().log(log_id, level, __FILE__, __LINE__, __func__, format, ##__VA_ARGS__); \
        } \
    } while (0)

#define ZRLOG_TRACE(format, ...) ZRLOG_BODY(zrlog::LogLevel::TRACE, format, ##__VA_ARGS__)
#define ZRLOG_DEBUG(format, ...) ZRLOG_BODY(zrlog::LogLevel::DEBUG, format, ##__VA_ARGS__)
#define ZRLOG_INFO(format, ...)  ZRLOG_BODY(zrlog::LogLevel::INFO,  format, ##__VA_ARGS__)
#define ZRLOG_WARN(format, ...)  ZRLOG_BODY(zrlog::LogLevel::WARN,  format, ##__VA_ARGS__)
#define ZRLOG_ERROR(format, ...) ZRLOG_BODY(zrlog::LogLevel::ERR,   format, ##__VA_ARGS__)
#define ZRLOG_FATAL(format, ...) ZRLOG_BODY(zrlog::LogLevel::FATAL, format, ##__VA_ARGS__)
