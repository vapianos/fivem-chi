#include "core/lua_runtime.h"
#include "core/module.h"
#include "core/scanner.h"
#include "features/native.h"
#include "utils/log.h"
#include "utils/paths.h"

#include <Windows.h>
#include <cstring>
#include <mutex>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

using lua_pushcclosure_t = void (*)(lua_State*, int (*)(lua_State*), int);
using lua_pcallk_t       = int  (*)(lua_State*, int, int, int, intptr_t, void*);
using lua_loadbufferx_t  = int  (*)(lua_State*, const char*, size_t, const char*, const char*);
using lua_tolstring_t    = const char* (*)(lua_State*, int, size_t*);
using lua_pushstring_t   = const char* (*)(lua_State*, const char*);
using lua_pushinteger_t  = void (*)(lua_State*, long long);
using lua_createtable_t  = void (*)(lua_State*, int, int);
using lua_setfield_t     = void (*)(lua_State*, int, const char*);
using lua_gettop_t       = int  (*)(lua_State*);
using lua_settop_t       = void (*)(lua_State*, int);

constexpr int kGlobalsIndex = -1001002;

struct LuaApi {
    lua_loadbufferx_t  loadbufferx = nullptr;
    lua_pcallk_t       pcallk      = nullptr;
    lua_pushstring_t   pushstring  = nullptr;
    lua_createtable_t  createtable = nullptr;
    lua_setfield_t     setfield    = nullptr;
    lua_pushcclosure_t pushcclosure = nullptr;
    lua_tolstring_t    tolstring   = nullptr;
    lua_pushinteger_t  pushinteger = nullptr;
    lua_gettop_t       gettop      = nullptr;
    lua_settop_t       settop      = nullptr;

    bool ok() const { return loadbufferx && pcallk && tolstring && gettop && settop; }
    bool canRegister() const {
        return ok() && createtable && setfield && pushcclosure && pushinteger && pushstring;
    }
};

struct Sig { const char* name; const char* pattern; void** slot; bool optional; };

LuaApi               g_api;
std::recursive_mutex g_mutex;
lua_State*           g_state = nullptr;
bool                 g_ready = false;
std::vector<std::pair<std::string, std::string>> g_pending;

std::vector<ModuleInfo> LuaModules() {
    std::vector<ModuleInfo> out;
    for (auto& m : modules::All()) {
        std::string l = m.name;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)::tolower(c); });
        if (l.find("lua") != std::string::npos) out.push_back(m);
    }
    return out;
}

bool Resolve() {
    const auto mods = LuaModules();
    if (mods.empty()) { LogE("[lua] ningun modulo con \"lua\""); return false; }

    HMODULE host = nullptr;
    for (auto& m : mods) {
        HMODULE h = reinterpret_cast<HMODULE>(m.base);
        if (h && GetProcAddress(h, "luaL_loadbufferx")) { host = h; break; }
    }
    if (!host) { LogE("[lua] ningun modulo exporta luaL_loadbufferx"); return false; }
    Log("[lua] modulo anfitrion %p", reinterpret_cast<void*>(host));

    auto byName = [&](const char* n) { return reinterpret_cast<uintptr_t>(GetProcAddress(host, n)); };
    auto byPattern = [&](const char* pat) -> uintptr_t {
        for (auto& m : mods) {
            if (reinterpret_cast<HMODULE>(m.base) != host) continue;
            return scanner::FindIn(m, pat);
        }
        return 0;
    };

    static const Sig sigs[] = {
        { "luaL_loadbufferx", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30", (void**)&g_api.loadbufferx, false },
        { "lua_pcallk",       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20", (void**)&g_api.pcallk, false },
        { "lua_tolstring",    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B E9", (void**)&g_api.tolstring, false },
        { "lua_gettop",       "48 8B 41 10 48 2B 41 08 48 C1 F8 04 C3", (void**)&g_api.gettop, false },
        { "lua_settop",       "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B DA 85 D2 78 12", (void**)&g_api.settop, false },
        { "lua_pushstring",   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 75", (void**)&g_api.pushstring, true },
        { "lua_createtable",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B D9 48 63 FA 48 85 D2 7F", (void**)&g_api.createtable, true },
        { "lua_setfield",     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 74 1E 48 8B F2", (void**)&g_api.setfield, true },
        { "lua_pushcclosure", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B D9 48 8B FA 48 63 F1", (void**)&g_api.pushcclosure, true },
        { "lua_pushinteger",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B F2 48 85 D2", (void**)&g_api.pushinteger, true },
    };

    for (auto& sig : sigs) {
        uintptr_t hit = byName(sig.name);
        if (!hit) hit = byPattern(sig.pattern);
        if (!hit) {
            if (sig.optional) Log("firma opcional ausente: %s", sig.name);
            else LogE("firma no encontrada: %s", sig.name);
            continue;
        }
        *sig.slot = reinterpret_cast<void*>(hit);
        Log("[lua] %s en %p", sig.name, reinterpret_cast<void*>(hit));
    }
    return g_api.ok();
}

int api_log(lua_State* L) {
    size_t len = 0;
    const char* text = g_api.tolstring(L, 1, &len);
    if (text) Log("[lua] %.*s", static_cast<int>(len), text);
    return 0;
}

int api_version(lua_State* L) { g_api.pushstring(L, "fivem-cheat 1.0"); return 1; }
int api_mouse1(lua_State* L) { g_api.pushinteger(L, GetAsyncKeyState(0x01) & 0x8000 ? 1 : 0); return 1; }

void RegisterInternal(lua_State* L) {
    g_api.createtable(L, 0, 6);
    g_api.pushcclosure(L, api_log, 0);    g_api.setfield(L, -2, "log");
    g_api.pushcclosure(L, api_version, 0);g_api.setfield(L, -2, "version");
    g_api.pushcclosure(L, api_mouse1, 0); g_api.setfield(L, -2, "mouse1");
    native::addLuaFields(L);
    g_api.setfield(L, kGlobalsIndex, "cheat");
    g_api.settop(L, 0);
}
void CallOnLuaStateSafe(lua_State* L) {
    __try { native::onLuaState(L); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogE("excepcion instalando el script"); }
}
} // namespace

namespace lua {

bool init() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_ready) return true;
    if (!Resolve()) { LogE("no se pudieron resolver las firmas de Lua"); return false; }
    g_ready = true;
    Log("runtime Lua listo%s", g_api.canRegister() ? "" : " (sin tabla cheat)");
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

void AddField(lua_State* L, const char* name, int (*fn)(lua_State*)) {
    if (!L || !fn || !g_api.canRegister()) return;
    g_api.pushcclosure(L, fn, 0);
    g_api.setfield(L, -2, name);
}

const char* GetStringArg(lua_State* L, int idx, size_t& len) {
    len = 0;
    if (!g_api.tolstring) return nullptr;
    return g_api.tolstring(L, idx, &len);
}

void PushInteger(lua_State* L, long long v) {
    if (L && g_api.pushinteger) g_api.pushinteger(L, v);
}

void RegisterApi(lua_State* L) {
    if (!L || !g_api.canRegister()) return;
    const int top = g_api.gettop(L);
    RegisterInternal(L);
    g_api.settop(L, top);
}

void OnLoadBuffer(lua_State* L) {
    if (!L || !g_ready) return;

    bool first = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_state != L) { g_state = L; first = true; }
    }
    if (!first) return;

    RegisterApi(L);
    Log("estado Lua capturado (%p)", L);

    CallOnLuaStateSafe(L);

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
    if (!in.is_open()) { LogE("no se pudo abrir %s", paths::ToUtf8(file).c_str()); return false; }
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
    { std::lock_guard<std::recursive_mutex> lock(g_mutex); queue.swap(g_pending); }
    for (auto& [name, code] : queue) RunString(code, name);
}

size_t PendingCount() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_pending.size();
}
} // namespace lua