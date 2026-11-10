#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ModuleInfo {
    uintptr_t   base = 0;
    size_t      size = 0;
    std::string name;

    bool valid() const { return base != 0 && size != 0; }
};

namespace modules {
ModuleInfo Main();
ModuleInfo ByName(const std::string& name);
ModuleInfo Game();                       // busca el módulo del juego (FiveM/GTAProcess/GTA5)
bool       WaitForGame(int timeoutMs);
std::vector<ModuleInfo> All();
}