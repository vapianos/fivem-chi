#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <string>
#include "utils/log.h"
#include "utils/paths.h"
#include "core/config.h"
#include "features/native.h"
#include "render/overlay.h"

static HMODULE   g_self     = nullptr;
static uintptr_t g_base     = 0;
static size_t    g_size     = 0;
static volatile LONG g_unloadReq = 0;

static void BootMark(const char* text) {
    wchar_t dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
        lstrcatW(dir, L"\\FiveMCheat");
    } else if (!GetTempPathW(MAX_PATH, dir)) {
        return;
    } else {
        lstrcatW(dir, L"FiveMCheat");
    }
    CreateDirectoryW(dir, nullptr);

    const std::wstring path = std::wstring(dir) + L"\\boot.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024]{};
    const int n = wsprintfA(line, "[%02d:%02d:%02d.%03d] %s\r\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
    DWORD put = 0;
    WriteFile(h, line, (DWORD)n, &put, nullptr);
    CloseHandle(h);
}

template <typename F>
static bool Stage(const char* name, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        const std::string line = std::string(name) + " EXC: " + e.what();
        BootMark(line.c_str());
        LogE("[core] %s excepcion: %s", name, e.what());
    } catch (...) {
        const std::string line = std::string(name) + " EXC desconocida";
        BootMark(line.c_str());
        LogE("[core] %s excepcion desconocida", name);
    }
    return false;
}

static void CaptureRange() {
    if (!g_self) return;
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(g_self);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<uint8_t*>(g_self) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_base = reinterpret_cast<uintptr_t>(g_self);
    g_size = nt->OptionalHeader.SizeOfImage;
}

static bool ModuleNameOf(uintptr_t addr, char* out, size_t outLen, uintptr_t* modBase) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        if (GetModuleFileNameA(mod, out, (DWORD)outLen)) {
            if (modBase) *modBase = reinterpret_cast<uintptr_t>(mod);
            return true;
        }
    }
    if (g_size && addr >= g_base && addr < g_base + g_size) {
        GetModuleFileNameA(g_self, out, (DWORD)outLen);
        if (modBase) *modBase = g_base;
        return true;
    }
    return false;
}

static void LogFrame(const char* prefix, uintptr_t addr) {
    char path[MAX_PATH]{};
    uintptr_t mod = 0;
    if (ModuleNameOf(addr, path, sizeof(path), &mod)) {
        if (!path[0]) lstrcpyA(path, "(payload)");
        LogE("%s %p %s+0x%llX", prefix, reinterpret_cast<void*>(addr), path,
             static_cast<unsigned long long>(addr - mod));
    } else {
        LogE("%s %p (fuera de modulo)", prefix, reinterpret_cast<void*>(addr));
    }
}

static bool SelfImportsRuntime() {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(g_self);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<uint8_t*>(g_self) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size) return false;
    auto* imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<uint8_t*>(g_self) + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* n = reinterpret_cast<const char*>(g_self) + imp->Name;
        if (!_stricmp(n, "MSVCP140.dll") || !_stricmp(n, "VCRUNTIME140.dll")) return true;
    }
    return false;
}

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);

static void WriteDump(EXCEPTION_POINTERS* ep) {
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) { LogE("sin dbghelp, sin volcado"); return; }

    auto write = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!write) { FreeLibrary(dbg); return; }

    const std::wstring file = paths::DataDirW() + L"\\crash.dmp";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { FreeLibrary(dbg); return; }

    void* info = nullptr;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        info = &mei;
    }

    const DWORD type = MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory;
    if (write(GetCurrentProcess(), GetCurrentProcessId(), h, type, info, nullptr, nullptr))
        LogE("volcado escrito en %s", paths::ToUtf8(file).c_str());
    else
        LogE("MiniDumpWriteDump fallo: %lu", GetLastError());

    CloseHandle(h);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    const auto* rec = ep->ExceptionRecord;
    BootMark("CRASH");
    LogE("=== CRASH 0x%08X", rec->ExceptionCode);
    LogFrame("  rip", reinterpret_cast<uintptr_t>(rec->ExceptionAddress));

    void* frames[24]{};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
        LogFrame("  #", reinterpret_cast<uintptr_t>(frames[i]));

    WriteDump(ep);
    LogShutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI UnloadThread(LPVOID) {
    Sleep(150);
    overlay::shutdown();
    native::shutdown();
    BootMark("descargado");
    Log("[core] descargado");
    LogShutdown();
    ExitThread(0);
    return 0;
}

static DWORD WINAPI NativesThread(LPVOID) {
    Sleep(2000);
    if (!config::Get().nativesEnabled) { BootMark("nativas desactivadas"); return 0; }
    if (Stage("natives", [] { return native::init(); })) BootMark("nativas ok");
    else BootMark("nativas fallo");
    return 0;
}

static DWORD WINAPI Init(LPVOID) {
    BootMark("Init arranca");
    Sleep(60);

    paths::EnsureDirW(paths::DataDirW());
    LogInit();
    BootMark("log inicializado");
    SetUnhandledExceptionFilter(CrashFilter);
    CaptureRange();

    Log("[core] init pid=%lu base=%p size=0x%zX", GetCurrentProcessId(),
        reinterpret_cast<void*>(g_base), g_size);
    Log(SelfImportsRuntime() ? "[core] CRT DINAMICA (mal)" : "[core] CRT estatica, correcto");

    Stage("config", [] { config::Load(); return true; });

    bool window = false;
    Stage("ventana", [&] {
        for (int i = 0; i < 480 && !window; ++i) {
            window = FindWindowW(L"grcWindow", nullptr) != nullptr;
            if (!window) Sleep(250);
        }
        BootMark(window ? "ventana encontrada" : "sin ventana en 120s");
        return window;
    });

    if (!window) BootMark("sin ventana, el overlay no se puede armar");

    if (config::Get().overlayEnabled) {
        Sleep(700);
        bool armed = false;
        for (int i = 0; i < 40 && !armed; ++i) {
            Stage("overlay", [&] { armed = overlay::init(); return armed; });
            if (!armed) Sleep(500);
        }
        BootMark(armed ? "overlay armado" : "overlay fallo");
    } else {
        BootMark("overlay desactivado en config");
    }

    if (window) CloseHandle(CreateThread(nullptr, 0, NativesThread, nullptr, 0, nullptr));

    BootMark("listo");
    Log("[core] listo");
    return 0;
}

extern "C" __declspec(dllexport) void Unload() {
    if (InterlockedExchange(&g_unloadReq, 1)) return;
    CloseHandle(CreateThread(nullptr, 0, UnloadThread, nullptr, 0, nullptr));
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, Init, nullptr, 0, nullptr));
    }
    return TRUE;
}