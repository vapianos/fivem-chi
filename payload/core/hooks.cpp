#include "core/hooks.h"
#include "core/lua_runtime.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <MinHook.h>

namespace {
using loadbufferx_t = int (*)(void* L, const char* buff, size_t size, const char* name, const char* mode);

loadbufferx_t o_loadbufferx = nullptr;
void*         g_target      = nullptr;
bool          g_installed   = false;

// Sin TLS: bajo manual map el indice TLS del modulo no queda inicializado y
// cualquier thread_local revienta en el primer acceso. Guard por interlocked.
volatile LONG g_inside = 0;

// 0 sin probar, 1 firma buena, -1 firma mala. Si una firma generica cayo en la
// funcion equivocada, llamar a gettop sobre basura seria un paseo; se valida en
// la primera invocacion real y si no cuadra el hook queda mudo (sigue
// reenviando a la original, que es lo importante).
volatile LONG g_validated = 0;

int hk_loadbufferx(void* L, const char* buff, size_t size, const char* name, const char* mode) {
    if (L && o_loadbufferx && InterlockedExchange(&g_inside, 1) == 0) {
        __try {
            if (g_validated == 0) {
                const int top = lua::probeTop(static_cast<lua_State*>(L));
                g_validated = (top >= 0 && top <= 4096) ? 1 : -1;
                if (g_validated == 1) Log("[hooks] hook validado (top=%d)", top);
                else                  LogE("[hooks] hook NO valido (top=%d), queda mudo", top);
            }
            if (g_validated == 1)
                lua::OnLoadBuffer(static_cast<lua_State*>(L));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_validated = -1;
            LogE("excepcion en OnLoadBuffer: hook descartado");
        }
        InterlockedExchange(&g_inside, 0);
    }
    return o_loadbufferx(L, buff, size, name, mode);
}
} // namespace

namespace hooks {

bool init() {
    if (g_installed) return true;

    void* at = lua::target();
    if (!at) {
        // El runtime no esta resuelto: se resuelve aqui, porque el hook es lo
        // que da acceso al estado y no debe depender de que init() ya se haya
        // llamado antes en otro hilo.
        if (!lua::init()) { LogE("[hooks] runtime Lua no disponible, sin hook"); return false; }
        at = lua::target();
        if (!at) { LogE("[hooks] sin objetivo luaL_loadbufferx"); return false; }
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LogE("MH_Initialize fallo: %d", st);
        return false;
    }

    g_target = at;
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

    g_validated = 0;
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
    g_validated = 0;
    Log("hook retirado");
}

bool installed() { return g_installed; }
} // namespace hooks