#include "inject.h"
#include "manualmap.h"
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <fstream>
#include <cstdio>

namespace {
uintptr_t g_lastBase = 0;
DWORD     g_lastPid  = 0;

void WriteMarker() {
    wchar_t appdata[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH)) return;

    std::wstring dir = std::wstring(appdata) + L"\\FiveMCheat";
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0';

    HANDLE h = CreateFileW((dir + L"\\dir.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    const int n = WideCharToMultiByte(CP_UTF8, 0, exe, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string utf8(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, &utf8[0], n, nullptr, nullptr);
        DWORD put = 0;
        WriteFile(h, utf8.c_str(), (DWORD)utf8.size(), &put, nullptr);
    }
    CloseHandle(h);
}
} // namespace

DWORD inj::FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

DWORD inj::FindGameProcess() {
    // El proceso que nos interesa es el que tiene cargado el modulo del juego,
    // no el launcher de FiveM.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    DWORD found = 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"FiveM.exe") && _wcsnicmp(pe.szExeFile, L"FiveM", 5))
                continue;

            HANDLE msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                    pe.th32ProcessID);
            if (msnap == INVALID_HANDLE_VALUE) continue;

            MODULEENTRY32W me{ sizeof(me) };
            if (Module32FirstW(msnap, &me)) {
                do {
                    if (!_wcsnicmp(me.szModule, L"GTA5", 4)) { found = pe.th32ProcessID; break; }
                } while (Module32NextW(msnap, &me));
            }
            CloseHandle(msnap);
            if (found) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool CrtInject(HANDLE proc, const std::wstring& dll, std::string& err) {
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { err = "VirtualAllocEx fallo"; return false; }
    if (!WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        err = "WriteProcessMemory fallo";
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        return false;
    }

    auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, mem, 0, nullptr);
    if (!th) { err = "CreateRemoteThread fallo"; VirtualFreeEx(proc, mem, 0, MEM_RELEASE); return false; }

    WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    if (!code) { err = "LoadLibrary devolvio 0"; return false; }
    return true;
}

bool inj::Inject(DWORD pid, const std::wstring& dll, Method m, std::string& err) {
    // PROCESS_ALL_ACCESS se rechaza en algunos procesos; se degrada si falla.
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
    if (!proc) { err = "OpenProcess fallo (ejecuta como admin)"; return false; }

    WriteMarker();

    bool ok = false;
    if (m == Method::ManualMap) {
        std::ifstream f(dll, std::ios::binary);
        if (!f) { err = "no se pudo leer el DLL"; CloseHandle(proc); return false; }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
        uintptr_t base = 0;
        ok = manualmap::MapEx(proc, data, err, &base);
        if (ok) { g_lastBase = base; g_lastPid = pid; }
    } else {
        ok = CrtInject(proc, dll, err);
    }

    CloseHandle(proc);
    return ok;
}

bool inj::Eject(DWORD pid, std::string& err) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
    if (!proc) { err = "OpenProcess fallo"; return false; }

    // Manual map no aparece en la lista de modulos: se usa la base recordada
    // y el export Unload del propio payload.
    if (g_lastBase && g_lastPid == pid) {
        const uintptr_t base = g_lastBase;
        g_lastBase = 0;
        g_lastPid = 0;

        // Unload esta exportado: RVA leido de la copia en disco, no del proceso.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = L'\0';
        std::wstring dllPath = std::wstring(exe) + L"\\payload\\cheat.dll";

        uintptr_t rva = 0;
        std::ifstream f(dllPath, std::ios::binary);
        if (f) {
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
            if (data.size() > 0x200) {
                auto* dos = (PIMAGE_DOS_HEADER)data.data();
                auto* nt = (PIMAGE_NT_HEADERS64)(data.data() + dos->e_lfanew);
                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                if (dir.Size) {
                    auto* exp = (PIMAGE_EXPORT_DIRECTORY)(data.data() + dir.VirtualAddress);
                    auto* names = (DWORD*)(data.data() + exp->AddressOfNames);
                    auto* ords  = (WORD*)(data.data() + exp->AddressOfNameOrdinals);
                    auto* funcs = (DWORD*)(data.data() + exp->AddressOfFunctions);
                    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                        const char* n = (const char*)data.data() + names[i];
                        if (!strcmp(n, "Unload")) { rva = funcs[ords[i]]; break; }
                    }
                }
            }
        }

        if (!rva) { err = "no se encontro el export Unload en el DLL"; CloseHandle(proc); return false; }

        auto fp = (LPTHREAD_START_ROUTINE)(base + rva);
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, fp, nullptr, 0, nullptr);
        if (!th) { err = "CreateRemoteThread fallo"; CloseHandle(proc); return false; }
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
        CloseHandle(proc);
        return true;
    }

    // Fallback para LoadLibrary
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_64BIT)) {
        err = "EnumProcessModulesEx fallo"; CloseHandle(proc); return false;
    }

    uintptr_t base = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t name[MAX_PATH]{};
        GetModuleBaseNameW(proc, mods[i], name, MAX_PATH);
        if (!_wcsicmp(name, L"cheat.dll")) { base = (uintptr_t)mods[i]; break; }
    }
    if (!base) { err = "cheat.dll no esta cargado"; CloseHandle(proc); return false; }

    auto fp = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, fp, (void*)base, 0, nullptr);
    if (!th) { err = "CreateRemoteThread fallo"; CloseHandle(proc); return false; }
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    CloseHandle(proc);
    return true;
}

bool inj::IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}