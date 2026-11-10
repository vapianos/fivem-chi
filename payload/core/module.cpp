#include "core/module.h"
#include "utils/log.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace {
bool HasAny(const std::string& hay, const std::vector<std::string>& needles) {
    for (auto& n : needles) {
        if (hay.find(n) != std::string::npos)
            return true;
    }
    return false;
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
    for (auto& m : All()) {
        if (m.name.size() >= name.size() &&
            _stricmp(m.name.c_str(), name.c_str()) == 0)
            return m;
    }
    return {};
}

ModuleInfo Game() {
    // 1) módulo principal del proceso
    ModuleInfo main = Main();
    std::string lower = main.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("fivem") != std::string::npos || lower.find("gta") != std::string::npos)
        return main;

    // 2) módulos secundarios típicos de CitizenFX
    for (auto& m : All()) {
        std::string n = m.name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (HasAny(n, { "fivem", "gtaprocess", "gta5.exe", "gta5" }))
            return m;
    }
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