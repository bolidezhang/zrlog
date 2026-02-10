#pragma once

#include <vector>
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
#include <charconv> // C++17 std::to_chars

// ---------------------------------------------------------------------------
// 平台差异处理 & 内部宏定义
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN  // 排除不常用的头文件，加快编译速度
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <share.h>
#include <intrin.h> // For __rdtsc, _mm_pause

#define ZRLOG_OPEN(path, flags, mode)   _sopen(path, flags, _SH_DENYNO, mode)
#define ZRLOG_WRITE(fd, data, len)      _write(fd, data, (unsigned int)len)
#define ZRLOG_CLOSE(fd)                 _close(fd)
#define ZRLOG_FLUSH_FILE(fd)            _commit(fd)
#define ZRLOG_O_FLAGS                   (_O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY)
#define ZRLOG_S_FLAGS                   (_S_IREAD | _S_IWRITE)
#define ZRLOG_CPU_PAUSE()               _mm_pause()
#define ZRLOG_FAST_THREAD_LOCAL         __declspec(thread)

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
#include <sys/syscall.h> // For SYS_gettid
#include <sys/types.h>

#ifdef __x86_64__
#include <x86intrin.h> // For __rdtsc
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
// Fallback for non-x86/ARM64
inline uint64_t zrlog_rdtsc() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}
#endif

inline void zrlog_localtime(const time_t* timer, struct tm* buf) {
    localtime_r(timer, buf);
}
#endif

namespace zrlog {

    // ---------------------------------------------------------------------------
    // 内部细节
    // ---------------------------------------------------------------------------
    namespace detail {
        // 基础值解码
        template <typename T>
        typename std::enable_if<!std::is_same<T, const char*>::value, T>::type
            decode_val(char*& ptr) {
            T val;
            std::memcpy(&val, ptr, sizeof(T));
            ptr += sizeof(T);
            return val;
        }

        // 字符串解码
        template <typename T>
        typename std::enable_if<std::is_same<T, const char*>::value, const char*>::type
            decode_val(char*& ptr) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            const char* str = ptr + sizeof(uint32_t);
            ptr += sizeof(uint32_t) + len;
            return str;
        }

        // Tuple 构建器
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

    class SpinMutex
    {
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

    class FileHelper {
    public:
        ~FileHelper() {
            close();
        }

        bool open(const std::string& path) {
            if (fd_ != -1) {
                close();
            }
            fd_ = ZRLOG_OPEN(path.c_str(), ZRLOG_O_FLAGS, ZRLOG_S_FLAGS);
            return fd_ != -1;
        }

        void close() {
            if (fd_ != -1) {
                ZRLOG_CLOSE(fd_);
                fd_ = -1;
            }
        }

        void write(const char* data, size_t size) {
            if (fd_ != -1) {
                ZRLOG_WRITE(fd_, data, size);
            }
        }

        void flush() {
            if (fd_ != -1) {
                ZRLOG_FLUSH_FILE(fd_);
            }
        }

    private:
        int fd_ = -1;
    };

    // ---------------------------------------------------------------------------
    // TSC Clock
    // ---------------------------------------------------------------------------
    class TscClock {
    public:
        static void init() {
            // 简单校准：测量一段间隔内的系统时间和TSC变化
            auto t0 = std::chrono::system_clock::now();
            uint64_t tsc0 = zrlog_rdtsc();

            // 睡眠 10ms 进行校准
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            auto t1 = std::chrono::system_clock::now();
            uint64_t tsc1 = zrlog_rdtsc();

            uint64_t ns0 = std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count();
            uint64_t ns1 = std::chrono::duration_cast<std::chrono::nanoseconds>(t1.time_since_epoch()).count();

            base_ns_ = ns1;
            base_tsc_ = tsc1;

            // 计算每个 TSC tick 对应的纳秒数
            double ns_delta = static_cast<double>(ns1 - ns0);
            double tsc_delta = static_cast<double>(tsc1 - tsc0);
            if (tsc_delta > 0) {
                ns_per_tick_ = ns_delta / tsc_delta;
            }
            else {
                ns_per_tick_ = 1.0;
            }
        }

        static inline uint64_t now() {
            uint64_t tsc = zrlog_rdtsc();
            // 防止乱序导致的时光倒流（极罕见，但在旧CPU可能）
            if (tsc < base_tsc_) {
                return base_ns_;
            }
            return base_ns_ + static_cast<uint64_t>((tsc - base_tsc_) * ns_per_tick_);
        }

    private:
        static uint64_t base_tsc_;
        static uint64_t base_ns_;
        static double ns_per_tick_;
    };

    uint64_t TscClock::base_tsc_ = 0;
    uint64_t TscClock::base_ns_ = 0;
    double   TscClock::ns_per_tick_ = 1.0;

    // ---------------------------------------------------------------------------
    // 线程ID获取
    // ---------------------------------------------------------------------------
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
        TRACE = 0,
        DEBUG,
        INFO,
        WARN,
        ERR,
        FATAL
    };

    inline const char* loglevel_to_string(LogLevel level) {
        switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERR:   return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
        }
    }

    // ---------------------------------------------------------------------------
    // NanoLogger 主类
    // ---------------------------------------------------------------------------

    class NanoLogger {
    public:
        static const size_t IO_BUFFER_SIZE = 64 * 1024;
        static const uint32_t PER_THREAD_QUOTA = 256;

        // 日志头格式: yyyy-mm-dd hh:mm:ss.123456789 INFO thread_id file:line
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
            const char* format;
            std::string full_format;
            DecoderFn   decoder;
        };

        struct LogEntryHeader {
            uint32_t total_size;
            uint32_t log_id;
            uint64_t timestamp;
            uint64_t thread_id;
        };

        // -----------------------------------------------------------------------
        // IOBuffer
        // -----------------------------------------------------------------------
        class IOBuffer {
        public:
            IOBuffer(FileHelper* f) : file_(f) {
            }

            void append(const char* src, size_t len) {
                if (pos_ + len > IO_BUFFER_SIZE) {
                    flush_to_os();
                }
                if (len > IO_BUFFER_SIZE) {
                    file_->write(src, len);
                    return;
                }
                std::memcpy(data_ + pos_, src, len);
                pos_ += len;
            }

            void flush_to_os() {
                if (pos_ > 0) {
                    file_->write(data_, pos_);
                    pos_ = 0;
                }
            }

            void flush_force() {
                flush_to_os();
                file_->flush();
            }

            char* current_ptr() {
                return data_ + pos_;
            }
            size_t available_size() const {
                return IO_BUFFER_SIZE - pos_;
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
                auto res = std::to_chars(current_ptr(), data_ + IO_BUFFER_SIZE, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
            }
            IOBuffer& operator<<(float val) {
                if (available_size() < 64) {
                    flush_to_os();
                }
                auto res = std::to_chars(current_ptr(), data_ + IO_BUFFER_SIZE, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
            }
            IOBuffer& operator<<(double val) {
                if (available_size() < 64) {
                    flush_to_os();
                }
                auto res = std::to_chars(current_ptr(), data_ + IO_BUFFER_SIZE, val);
                if (res.ec == std::errc()) {
                    advance(res.ptr - current_ptr());
                }
                return *this;
            }

        private:
            char data_[IO_BUFFER_SIZE];
            size_t pos_ = 0;
            FileHelper* file_ = nullptr;
        };

        static NanoLogger& instance() {
            static NanoLogger logger;
            return logger;
        }

        bool init(const std::string& filename, LogLevel level) {
            TscClock::init();
            log_level_ = level;

            if (!log_thread_running_) {
                if (!log_file_.open(filename)) {
                    return false;
                }
                log_thread_running_ = true;
                log_thread_ = std::thread(&NanoLogger::poll_routine, this);
            }
            return true;
        }

        void fini() {
            if (log_thread_running_) {
                log_thread_running_ = false;
                if (log_thread_.joinable()) {
                    log_thread_.join();
                }
                log_file_.close();
            }
        }

        inline bool check_level(LogLevel level) const {
            return level >= log_level_;
        }

        // ========================================================================
        // Frontend
        // ========================================================================
        template<typename... Args>
        bool log(uint32_t& log_id, LogLevel level, const char* file, uint32_t line,
            const char* func, const char* format, Args&&... args) {

            if (log_id == 0) {
                log_id = register_log_meta<Args...>(level, file, line, func, format);
            }

            ThreadBuffer *buffer = get_thread_buffer();
            if (!buffer) {
                return false;
            }

            uint32_t args_size = calculate_args_size(args...);
            uint32_t total_size = sizeof(LogEntryHeader) + args_size;
            char *ptr = buffer->alloc(total_size);
            if (ptr) {
                LogEntryHeader *header = reinterpret_cast<LogEntryHeader*>(ptr);
                header->total_size = total_size;
                header->log_id    = log_id;
                header->timestamp = TscClock::now();
                header->thread_id = get_thread_id();

                char* data_ptr = ptr + sizeof(LogEntryHeader);
                serialize_args(data_ptr, std::forward<Args>(args)...);

                buffer->commit(total_size);
            }
            else {
                //buffer没有足够的空间时,处理策略:
                //0:直接丢弃日志(目前)
                //1:阻塞式重试
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

            // 转换时间戳 (ns -> tm)
            time_t seconds = static_cast<time_t>(header.timestamp / 1000000000);
            uint64_t nanoseconds = header.timestamp % 1000000000;
            struct tm t;
            zrlog_localtime(&seconds, &t);

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
            ThreadBuffer() : buffer_(SIZE) {
            }

            char* alloc(uint32_t size) {
                uint32_t write = write_index_.load(std::memory_order_relaxed);
                uint32_t read = read_index_.load(std::memory_order_acquire);
                if (write + size >= SIZE) {
                    if (SIZE - write >= sizeof(LogEntryHeader)) {
                        LogEntryHeader padding; padding.log_id = PADDING_ID; padding.total_size = SIZE - write;
                        std::memcpy(&buffer_[write], &padding, sizeof(LogEntryHeader));
                        commit(SIZE - write);
                    }
                    return nullptr;
                }
                if (write >= read) {
                    if (write + size >= SIZE) {
                        return nullptr;
                    }
                }
                else {
                    if (write + size >= read) {
                        return nullptr;
                    }
                }
                return &buffer_[write];
            }
            void commit(uint32_t size) {
                uint32_t write = write_index_.load(std::memory_order_relaxed);
                uint32_t next = write + size; 
                if (next >= SIZE) {
                    next = 0;
                }
                write_index_.store(next, std::memory_order_release);
            }
            bool try_read(char*& out_ptr) {
                uint32_t read = read_index_.load(std::memory_order_relaxed);
                uint32_t write = write_index_.load(std::memory_order_acquire);
                if (read == write) {
                    return false;
                }
                LogEntryHeader *header = reinterpret_cast<LogEntryHeader*>(&buffer_[read]);
                if (header->log_id == PADDING_ID) {
                    read_index_.store(0, std::memory_order_release);
                    read = 0;
                    if (read == write) {
                        return false;
                    }
                    header = reinterpret_cast<LogEntryHeader*>(&buffer_[read]);
                }
                out_ptr = &buffer_[read];
                return true;
            }
            void consume(uint32_t size) {
                uint32_t read = read_index_.load(std::memory_order_relaxed);
                uint32_t next = read + size; 
                if (next >= SIZE) {
                    next = 0;
                }
                read_index_.store(next, std::memory_order_release);
            }

            bool should_deallocate() const {
                return should_deallocate_;
            }
            void mark_deallocate() {
                should_deallocate_ = true;
            }

        private:
            static const uint32_t SIZE = 4 * 1024 * 1024;
            static const uint32_t PADDING_ID = 0xFFFFFFFF;
            std::vector<char> buffer_;
            alignas(64) std::atomic<uint32_t> write_index_{ 0 };
            alignas(64) std::atomic<uint32_t> read_index_{ 0 };
            bool should_deallocate_ = false;
            friend NanoLogger;
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
        uint32_t register_log_meta(LogLevel level, const char* file, uint32_t line, const char* func, const char* format) {
            LogMeta new_log;
            new_log.level = level;
            new_log.file = file;
            new_log.line = line;
            new_log.func = func;
            new_log.format = format;
            new_log.full_format.reserve(256);
            new_log.full_format.append(HEAD_FORMATS);
            new_log.full_format.append(format);
            new_log.decoder = &generated_decoder<Args...>;
            std::lock_guard<SpinMutex> lock(log_metas_mutex_);
            uint32_t id = global_log_metas_.size() + 1;
            new_log.id = id;
            global_log_metas_.push_back(new_log);
            return id;
        }

        ThreadBuffer* get_thread_buffer() {
            if (thread_buffer_ == nullptr) {
                thread_buffer_ = new ThreadBuffer();
                (void)thread_buffer_destroyer_;
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                thread_buffers_.push_back(thread_buffer_);
            }
            return thread_buffer_;
        }

        bool consume_buffers_round_robin(std::vector<LogMeta>& local_log_metas, IOBuffer& io_buf) {
            bool any_data_processed = false;

            {
                size_t global_size = 0;
                std::lock_guard<SpinMutex> lock(log_metas_mutex_);
                global_size = global_log_metas_.size();
                if (global_size > local_log_metas.size()) {
                    local_log_metas.insert(local_log_metas.end(),
                        global_log_metas_.begin() + local_log_metas.size(),
                        global_log_metas_.end());
                }
            }
            {
                std::lock_guard<SpinMutex> lock(thread_buffers_mutex_);
                if (!thread_buffers_.empty()) {
                    thread_buffers_bg_.insert(thread_buffers_bg_.end(), thread_buffers_.begin(), thread_buffers_.end());
                    thread_buffers_.clear();
                }
            }

            auto it = thread_buffers_bg_.begin();
            while (it != thread_buffers_bg_.end()) {
                ThreadBuffer *tb = *it;
                char *ptr = nullptr;
                bool thread_active = false;
                uint32_t quota = PER_THREAD_QUOTA;

                while (quota > 0 && tb->try_read(ptr)) {
                    any_data_processed = true;
                    thread_active = true;
                    quota--;

                    LogEntryHeader* header = reinterpret_cast<LogEntryHeader*>(ptr);

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
            IOBuffer io_buf(&log_file_);

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
            global_log_metas_.reserve(1000);
            thread_buffers_.reserve(64);
            thread_buffers_bg_.reserve(64);
        }
        ~NanoLogger() {
            fini();
        }

        static ZRLOG_FAST_THREAD_LOCAL ThreadBuffer *thread_buffer_;
        static thread_local ThreadBufferDestroyer thread_buffer_destroyer_;

        SpinMutex log_metas_mutex_;
        std::vector<LogMeta> global_log_metas_;
        SpinMutex thread_buffers_mutex_;
        std::vector<ThreadBuffer*> thread_buffers_;
        std::vector<ThreadBuffer*> thread_buffers_bg_;
        FileHelper log_file_;
        std::thread log_thread_;

        volatile bool log_thread_running_ = false;
        LogLevel log_level_ = LogLevel::DEBUG;
    };

    ZRLOG_FAST_THREAD_LOCAL NanoLogger::ThreadBuffer *NanoLogger::thread_buffer_ = nullptr;
    thread_local NanoLogger::ThreadBufferDestroyer NanoLogger::thread_buffer_destroyer_;
}

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
