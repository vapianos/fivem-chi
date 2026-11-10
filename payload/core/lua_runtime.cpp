#include "core/lua_runtime.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <Windows.h>
#include <mutex>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

using lua_pushcclosure_t = void (*)(lua_State*, int (*)(lua_State*), int);
using lua_setglobal_t    = void (*)(lua_State*, const char*);
using lua_getglobal_t    = int  (*)(lua_State*, const char*);
using lua_pcallk_t       = int  (*)(lua_State*, int, int, int, intptr_t, void*);
using lua_loadbufferx_t  = int  (*)(lua_State*, const char*, size_t, const char*, const char*);
using lua_tolstring_t    = const char* (*)(lua_State*, int, size_t*);
using lua_pushstring_t   = const char* (*)(lua_State*, const char*);
using lua_pushinteger_t  = void (*)(lua_State*, long long);
using lua_pushboolean_t  = void (*)(lua_State*, int);
using lua_pushnil_t      = void (*)(lua_State*);
using lua_createtable_t  = void (*)(lua_State*, int, int);
using lua_setfield_t     = void (*)(lua_State*, int, const char*);
using lua_gettop_t       = int  (*)(lua_State*);
using lua_settop_t       = void (*)(lua_State*, int);
using lua_type_t         = int  (*)(lua_State*, int);

struct LuaApi {
    lua_loadbufferx_t  loadbufferx = nullptr;
    lua_pcallk_t       pcallk      = nullptr;
    lua_pushstring_t   pushstring  = nullptr;
    lua_setglobal_t    setglobal   = nullptr;
    lua_getglobal_t    getglobal   = nullptr;
    lua_createtable_t  createtable = nullptr;
    lua_setfield_t     setfield    = nullptr;
    lua_pushcclosure_t pushcclosure = nullptr;
    lua_tolstring_t    tolstring   = nullptr;
    lua_pushinteger_t  pushinteger = nullptr;
    lua_pushboolean_t  pushboolean = nullptr;
    lua_pushnil_t      pushnil     = nullptr;
    lua_gettop_t       gettop      = nullptr;
    lua_settop_t       settop      = nullptr;
    lua_type_t         type        = nullptr;

    bool ok() const {
        return loadbufferx && pcallk && pushstring && setglobal && getglobal &&
               createtable && setfield && pushcclosure && tolstring &&
               pushinteger && pushboolean && pushnil && gettop && settop && type;
    }
};

struct Sig {
    const char* name;
    const char* pattern;
    void** slot;
};

LuaApi            g_api;
// recursivo: RunString puede reentrar (un script que hace load()) y el lock
// se toma alrededor de la llamada a Lua.
std::recursive_mutex g_mutex;
lua_State*        g_state = nullptr;
bool              g_ready = false;
std::vector<std::pair<std::string, std::string>> g_pending;

// Modulos donde vive de verdad el runtime Lua de FiveM, por prioridad.
const char* kLuaModules[] = {
    "citizen-scripting-lua",
    "gta5",
    "gtaprocess",
    "adhesive",
    "citizen-resources-core",
    "citizen-resources-client",
};

bool Preferred(const std::string& name) {
    std::string l = name;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)::tolower(c); });
    for (const char* p : kLuaModules)
        if (l.find(p) != std::string::npos) return true;
    return false;
}

// Prioriza modulos conocidos y, dentro de cada uno, acepta el primer hit:
// exigir unicidad absoluta descarta modulos validos (hay firmas con 8 hits).
uintptr_t FindAnywhere(const char* pattern) {
    auto mods = modules::All();

    std::vector<ModuleInfo> ordered;
    ordered.reserve(mods.size());
    for (auto& m : mods) if (Preferred(m.name)) ordered.push_back(m);
    for (auto& m : mods) if (!Preferred(m.name)) ordered.push_back(m);

    for (auto& m : ordered) {
        if (uintptr_t hit = scanner::FindIn(m, pattern))
            return hit;
    }
    return 0;
}

bool Resolve() {
    static const Sig sigs[] = {
        { "luaL_loadbufferx", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30", (void**)&g_api.loadbufferx },
        { "lua_pcallk",       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20", (void**)&g_api.pcallk },
        { "lua_pushstring",   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 75", (void**)&g_api.pushstring },
        { "lua_setglobal",    "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 85 D2 75 08 48 8B 05", (void**)&g_api.setglobal },
        { "lua_getglobal",    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 75 08 48 8B 05", (void**)&g_api.getglobal },
        { "lua_createtable",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B D9 48 63 FA 48 85 D2 7F", (void**)&g_api.createtable },
        { "lua_setfield",     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 74 1E 48 8B F2", (void**)&g_api.setfield },
        { "lua_pushcclosure", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B D9 48 8B FA 48 63 F1", (void**)&g_api.pushcclosure },
        { "lua_tolstring",    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B E9", (void**)&g_api.tolstring },
        { "lua_pushinteger",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B F2 48 85 D2", (void**)&g_api.pushinteger },
        { "lua_pushboolean",  "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B FA 85 D2 74 04", (void**)&g_api.pushboolean },
        { "lua_pushnil",      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B 49 10 48 8B 41 08", (void**)&g_api.pushnil },
        { "lua_gettop",       "48 8B 41 10 48 2B 41 08 48 C1 F8 04 C3", (void**)&g_api.gettop },
        { "lua_settop",       "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B DA 85 D2 78 12", (void**)&g_api.settop },
        { "lua_type",         "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 63 FA 48 85 D2 7F 0D", (void**)&g_api.type },
    };

    bool all = true;
    for (auto& sig : sigs) {
        uintptr_t hit = FindAnywhere(sig.pattern);
        if (!hit) {
            LogE("firma no encontrada: %s", sig.name);
            all = false;
            continue;
        }
        *sig.slot = reinterpret_cast<void*>(hit);
    }
    return all;
}

// --- API expuesta a Lua -----------------------------------------------------

int api_log(lua_State* L) {
    size_t len = 0;
    const char* text = g_api.tolstring(L, 1, &len);
    if (text) Log("[lua] %.*s", static_cast<int>(len), text);
    return 0;
}

int api_version(lua_State* L) {
    g_api.pushstring(L, "fivem-cheat 1.0");
    return 1;
}

int api_getkey(lua_State* L) {
    g_api.pushinteger(L, GetAsyncKeyState(0x01) & 0x8000 ? 1 : 0);
    return 1;
}

void RegisterInternal(lua_State* L) {
    g_api.createtable(L, 0, 4);
    g_api.pushcclosure(L, api_log, 0);       g_api.setfield(L, -2, "log");
    g_api.pushcclosure(L, api_version, 0);   g_api.setfield(L, -2, "version");
    g_api.pushcclosure(L, api_getkey, 0);    g_api.setfield(L, -2, "mouse1");
    g_api.setglobal(L, "cheat");

    g_api.getglobal(L, "_G");
    g_api.pushstring(L, "fivem-cheat");
    g_api.setfield(L, -2, "__cheat");
    g_api.settop(L, 0);
}
} // namespace

namespace lua {

bool init() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_ready) return true;
    if (!Resolve()) {
        LogE("no se pudieron resolver todas las firmas de Lua");
        return false;
    }
    g_ready = true;
    Log("runtime Lua listo");
    return true;
}

void shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_pending.clear();
    g_state = nullptr;
    g_ready = false;
}

bool ready() { return g_ready && g_state != nullptr; }

lua_State* state() { return g_state; }

void RegisterApi(lua_State* L) {
    if (!L || !g_api.ok()) return;
    const int top = g_api.gettop(L);
    RegisterInternal(L);
    g_api.settop(L, top);
}

void OnLoadBuffer(lua_State* L) {
    if (!L || !g_ready) return;

    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_state == L) return;   // ya registrado
        g_state = L;
    }

    RegisterApi(L);
    Log("estado Lua capturado (%p)", L);
    FlushPending();
}

bool RunString(const std::string& code, const std::string& chunkName) {
    if (!ready() || !g_api.ok()) return false;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_state) return false;

    const int top = g_api.gettop(g_state);

    if (g_api.loadbufferx(g_state, code.c_str(), code.size(), chunkName.c_str(), "t") != 0) {
        size_t len = 0;
        const char* err = g_api.tolstring(g_state, -1, &len);
        LogE("error compilando %s: %.*s", chunkName.c_str(), static_cast<int>(len), err ? err : "?");
        g_api.settop(g_state, top);
        return false;
    }

    if (g_api.pcallk(g_state, 0, 0, 0, 0, nullptr) != 0) {
        size_t len = 0;
        const char* err = g_api.tolstring(g_state, -1, &len);
        LogE("error ejecutando %s: %.*s", chunkName.c_str(), static_cast<int>(len), err ? err : "?");
        g_api.settop(g_state, top);
        return false;
    }

    g_api.settop(g_state, top);
    return true;
}

bool RunFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        LogE("no se pudo abrir %s", paths::ToUtf8(file).c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return RunString(ss.str(), file.filename().string());
}

void PushPending(const std::string& name, std::string code) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_pending.emplace_back(name, std::move(code));
}

void FlushPending() {
    std::vector<std::pair<std::string, std::string>> queue;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        queue.swap(g_pending);
    }
    for (auto& [name, code] : queue)
        RunString(code, name);
}

size_t PendingCount() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_pending.size();
}
} // namespace lua