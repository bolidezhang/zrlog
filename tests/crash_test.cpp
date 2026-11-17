// 崩溃抢救验证：独立进程，子进程触发 SIGSEGV，父进程检查日志是否已落盘。
// 用法: crash_test.exe
// 仅 Windows 实现（用 CreateProcess 隔离崩溃）；其他平台编译为空壳直接返回 0。
#define WIN32_LEAN_AND_MEAN
#if defined(_WIN32)
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

#include "zrlog.h"

#if !defined(_WIN32)
int main() {
    std::printf("crash_test: skipped (Windows only)\n");
    return 0;
}
#else

static bool file_contains(const char* path, const char* needle) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str().find(needle) != std::string::npos;
}

// 子进程：初始化 zrlog，写日志，然后触发 SIGSEGV
static int crash_child_main(const char* logfile) {
    zrlog::install_crash_handler();

    zrlog::Config cfg;
    cfg.appender = zrlog::AppenderType::File;
    cfg.filename = logfile;
    cfg.level = zrlog::LogLevel::INFO;
    cfg.force_flush_interval_ms = 60000;  // 故意设很长：只有崩溃抢救能把它写下去
    cfg.crash_drain_wait_ms = 3000;
    ZRLOG_INIT_CONF(cfg);

    ZRLOG_INFO("BEFORE_CRASH {}", 1);
    ZRLOG_INFO("CRASH_MARKER {}", 42);

    volatile int* p = nullptr;
    *p = 1;  // SIGSEGV
    return 0;
}

int main(int argc, char** argv) {
    const char* logfile = "crash_test.log";

    if (argc > 1 && std::strcmp(argv[1], "--child") == 0) {
        return crash_child_main(logfile);
    }

    std::remove(logfile);

    // 用自身再次启动，作为子进程（避免父进程被信号杀死）
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);

    std::string cmd = std::string("\"") + exe + "\" --child";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL: CreateProcess err=%lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    const bool crashed = code != 0;
    const bool has_marker = file_contains(logfile, "CRASH_MARKER");
    const bool has_before = file_contains(logfile, "BEFORE_CRASH");

    std::printf("child_exit=0x%08lX crashed=%d\n", code, crashed ? 1 : 0);
    std::printf("log_has_BEFORE_CRASH=%d\n", has_before ? 1 : 0);
    std::printf("log_has_CRASH_MARKER=%d\n", has_marker ? 1 : 0);

    const bool ok = crashed && has_marker;
    std::printf("%s\n", ok ? "RESULT: PASS (crash-flush recovered logs)" : "RESULT: FAIL");
    return ok ? 0 : 1;
}

#endif  // _WIN32
