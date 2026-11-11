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

// Sin TLS: bajo manual map el indice TLS del modulo no queda inicializado y
// cualquier thread_local revienta en el primer acceso. Guard por interlocked.
volatile LONG g_inside = 0;

int hk_loadbufferx(void* L, const char* buff, size_t size, const char* name, const char* mode) {
    if (L && o_loadbufferx && InterlockedExchange(&g_inside, 1) == 0) {
        __try {
            lua::OnLoadBuffer(static_cast<lua_State*>(L));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogE("excepcion en OnLoadBuffer");
        }
        InterlockedExchange(&g_inside, 0);
    }
    return o_loadbufferx(L, buff, size, name, mode);
}

bool IsLuaModule(const std::string& n) {
    std::string l = n;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)::tolower(c); });
    return l.find("citizen-scripting-lua") != std::string::npos;
}

// Por nombre primero. Si no hay export, el patron generico de prologo puede
// coincidir con otra funcion del mismo modulo: se exige coincidencia unica y
// que caiga en seccion ejecutable.
uintptr_t Locate(const ModuleInfo& m, const char* pat) {
    HMODULE h = reinterpret_cast<HMODULE>(m.base);
    if (h) {
        if (auto p = GetProcAddress(h, "luaL_loadbufferx")) {
            Log("[hooks] luaL_loadbufferx por export en %s", m.name.c_str());
            return reinterpret_cast<uintptr_t>(p);
        }
    }

    size_t hits = 0;
    const uintptr_t at = scanner::FindUnique(m, pat, &hits);
    if (!at) {
        if (hits > 1) LogE("[hooks] patron ambiguo en %s (%zu coincidencias)", m.name.c_str(), hits);
        return 0;
    }
    if (!scanner::InExecutableSection(at)) return 0;
    Log("[hooks] luaL_loadbufferx por patron en %s", m.name.c_str());
    return at;
}
} // namespace

namespace hooks {

bool init() {
    if (g_installed) return true;

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

    for (auto& m : modules::All()) {
        if (!IsLuaModule(m.name)) continue;
        const uintptr_t at = Locate(m, pat);
        if (!at) continue;
        g_target = reinterpret_cast<void*>(at);
        Log("[hooks] objetivo %s+0x%llX", m.name.c_str(), (unsigned long long)(at - m.base));
        break;
    }

    if (!g_target) {
        LogE("no se encontro luaL_loadbufferx, sin hook");
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