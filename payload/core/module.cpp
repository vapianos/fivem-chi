#include "core/module.h"
#include "utils/log.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace {
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// FiveM renombra el ejecutable del juego en cada build (FiveM_b3889_GTAProcess.exe,
// FiveM_b3407_GTAProcess.exe...). Nunca buscar "gta5" a secas.
bool LooksLikeGame(const std::string& name) {
    const std::string l = Lower(name);
    return l.find("gtaprocess") != std::string::npos ||
           l.find("gta5") != std::string::npos ||
           l.find("gta5.exe") != std::string::npos;
}
} // namespace

namespace modules {

ModuleInfo Main() {
    ModuleInfo info{};
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return info;

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), base, &mi, sizeof(mi)))
        return info;

    info.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    info.size = mi.SizeOfImage;

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(base, path, MAX_PATH);
    std::wstring w = path;
    info.name.assign(w.begin(), w.end());
    return info;
}

std::vector<ModuleInfo> All() {
    std::vector<ModuleInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;

    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleInfo info{};
            info.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            info.size = me.modBaseSize;
            std::wstring w = me.szModule;
            info.name.assign(w.begin(), w.end());
            if (info.valid())
                out.push_back(std::move(info));
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

ModuleInfo ByName(const std::string& name) {
    const std::string want = Lower(name);
    for (auto& m : All()) {
        if (Lower(m.name) == want)
            return m;
    }
    return {};
}

ModuleInfo Game() {
    // 1) modulo principal: en el proceso del juego es FiveM_bXXXX_GTAProcess.exe
    ModuleInfo main = Main();
    if (LooksLikeGame(main.name)) {
        Log("[modules] modulo del juego (principal) %s base=%p size=0x%zX",
            main.name.c_str(), reinterpret_cast<void*>(main.base), main.size);
        return main;
    }

    // 2) cualquier modulo que sea el proceso del juego, aunque no sea el principal
    for (auto& m : All()) {
        if (LooksLikeGame(m.name)) {
            Log("[modules] modulo del juego (secundario) %s base=%p size=0x%zX",
                m.name.c_str(), reinterpret_cast<void*>(m.base), m.size);
            return m;
        }
    }

    LogE("[modules] sin modulo de juego (principal: %s)", main.name.c_str());
    return {};
}

bool WaitForGame(int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (GetTickCount64() < deadline) {
        if (Game().valid()) return true;
        Sleep(250);
    }
    return false;
}
} // namespace modules