#include "core/hooks.h"
#include "core/lua_runtime.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>

namespace {
using loadbufferx_t = int (*)(void* L, const char* buff, size_t size, const char* name, const char* mode);

loadbufferx_t o_loadbufferx = nullptr;
void*         g_target      = nullptr;
bool          g_installed   = false;

int hk_loadbufferx(void* L, const char* buff, size_t size, const char* name, const char* mode) {
    static thread_local bool inside = false;

    if (!inside && L && o_loadbufferx) {
        inside = true;
        __try {
            lua::OnLoadBuffer(static_cast<lua_State*>(L));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogE("excepcion en OnLoadBuffer");
        }
        inside = false;
    }

    return o_loadbufferx(L, buff, size, name, mode);
}

// Orden de preferencia: el runtime Lua de FiveM primero. "Interesting" solo
// sirve de comodin, no como criterio unico.
int ModulePriority(const std::string& n) {
    std::string l = n;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)::tolower(c); });

    if (l.find("citizen-scripting-lua") != std::string::npos) return 0;
    if (l.find("gtaprocess") != std::string::npos)           return 1;
    if (l.find("gta5") != std::string::npos)                 return 2;
    if (l.find("adhesive") != std::string::npos)             return 3;
    if (l.find("citizen") != std::string::npos)              return 4;
    if (l.find("fivem") != std::string::npos)                return 5;
    return 100;
}
} // namespace

namespace hooks {

bool init() {
    if (g_installed) return true;

    // Sin API de Lua resuelta el hook no aporta nada y solo añade riesgo.
    if (!lua::init()) {
        LogE("[hooks] runtime Lua no disponible, sin hook");
        return false;
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LogE("MH_Initialize fallo: %d", st);
        return false;
    }

    const char* pat = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30";

    auto mods = modules::All();
    std::stable_sort(mods.begin(), mods.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        return ModulePriority(a.name) < ModulePriority(b.name);
    });

    for (auto& m : mods) {
        const int prio = ModulePriority(m.name);
        if (prio >= 100) continue;

        // Primer hit dentro del modulo prioritario: la unicidad estricta
        // descartaba citizen-scripting-lua por tener varias coincidencias.
        const uintptr_t at = scanner::FindIn(m, pat);
        if (!at) continue;

        g_target = reinterpret_cast<void*>(at);
        Log("[hooks] luaL_loadbufferx en %s+0x%llX (prio %d)", m.name.c_str(),
            (unsigned long long)(at - m.base), prio);
        break;
    }

    if (!g_target) {
        LogE("no se encontro luaL_loadbufferx, se sigue sin hook");
        return false;
    }

    if (MH_CreateHook(g_target, &hk_loadbufferx, reinterpret_cast<void**>(&o_loadbufferx)) != MH_OK) {
        LogE("MH_CreateHook fallo");
        g_target = nullptr;
        return false;
    }

    if (MH_EnableHook(g_target) != MH_OK) {
        LogE("MH_EnableHook fallo");
        MH_RemoveHook(g_target);
        g_target = nullptr;
        o_loadbufferx = nullptr;
        return false;
    }

    g_installed = true;
    Log("hook luaL_loadbufferx instalado en %p", g_target);
    return true;
}

void shutdown() {
    if (!g_installed) return;

    if (g_target) {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
    }

    o_loadbufferx = nullptr;
    g_installed = false;
    Log("hook retirado");
}

bool installed() { return g_installed; }
} // namespace hooks