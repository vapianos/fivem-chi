#include "utils/log.h"
#include "utils/paths.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static SRWLOCK g_lock = SRWLOCK_INIT;

static void Out(const char* tag, const char* fmt, va_list ap) {
    char body[1024]{};
    vsnprintf(body, sizeof(body), fmt, ap);

    char line[1200]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][%s] %s\r\n",
                           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag, body);
    if (n <= 0) return;

    OutputDebugStringA(line);

    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        AcquireSRWLockExclusive(&g_lock);
        WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void LogInit() {
    const fs::path dir = paths::DataDir();
    paths::EnsureDir(dir);

    const fs::path file = paths::LogFile();
    g_file = CreateFileW(file.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                         nullptr, OPEN_ALWAYS, 0, nullptr);
}

void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); Out("info", fmt, ap); va_end(ap);
}

void LogE(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); Out("err", fmt, ap); va_end(ap);
}

void LogShutdown() {
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}