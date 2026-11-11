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

// Lua 5.4: no existe pseudoindice de globals. Se llega a _G por un chunk que
// recibe la tabla por argumento. Solo se usan firmas ya resueltas.
constexpr const char* kRegChunk = "local t = ...\ncheat = t\n";

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

LuaApi               g_api;
std::recursive_mutex g_mutex;
lua_State*           g_state = nullptr;
bool                 g_ready = false;
bool                 g_registered = false;
void*                g_target = nullptr;
std::vector<std::pair<std::string, std::string>> g_pending;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// Orden de preferencia: 1) modulos con pinta de runtime scripting (ahi vive
// Lua cuando el runtime esta en su propia DLL), 2) ejecutable del juego (ahi
// vive cuando va enlazado estatico), 3) el resto. El nombre solo PRIORIZA:
// FiveM no exporta la API de Lua, asi que nunca se exige un export.
std::vector<ModuleInfo> Ordered() {
    std::vector<ModuleInfo> first, game, rest;
    for (auto& m : modules::All()) {
        const std::string l = Lower(m.name);
        if (l.find("lua") != std::string::npos || l.find("scripting") != std::string::npos)
            first.push_back(m);
        else if (l.find("gtaprocess") != std::string::npos || l.find("gta5") != std::string::npos)
            game.push_back(m);
        else
            rest.push_back(m);
    }
    first.insert(first.end(), game.begin(), game.end());
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

struct Sig { const char* name; const char* pat; void** slot; };

static const Sig kSigs[] = {
    { "luaL_loadbufferx", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30", (void**)&g_api.loadbufferx },
    { "lua_pcallk",       "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20", (void**)&g_api.pcallk },
    { "lua_tolstring",    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 48 8B E9", (void**)&g_api.tolstring },
    { "lua_gettop",       "48 8B 41 10 48 2B 41 08 48 C1 F8 04 C3", (void**)&g_api.gettop },
    { "lua_settop",       "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B DA 85 D2 78 12", (void**)&g_api.settop },
    { "lua_pushstring",   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 75", (void**)&g_api.pushstring },
    { "lua_createtable",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B D9 48 63 FA 48 85 D2 7F", (void**)&g_api.createtable },
    { "lua_setfield",     "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 85 D2 74 1E 48 8B F2", (void**)&g_api.setfield },
    { "lua_pushcclosure", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B D9 48 8B FA 48 63 F1", (void**)&g_api.pushcclosure },
    { "lua_pushinteger",  "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B F2 48 85 D2", (void**)&g_api.pushinteger },
};

constexpr size_t kSigCount = sizeof(kSigs) / sizeof(kSigs[0]);

// Devuelve la primera coincidencia en seccion ejecutable y registra el RVA de
// todas cuando hay mas de una, para poder fijar la buena con el log delante.
uintptr_t PickIn(const ModuleInfo& m, const char* pat, size_t* hits) {
    *hits = 0;
    const auto all = scanner::FindAll(m, pat, 32);
    *hits = all.size();
    if (all.empty()) return 0;

    if (all.size() > 1) {
        for (auto a : all)
            Log("[lua]   candidato %s+0x%llX", m.name.c_str(),
                static_cast<unsigned long long>(a - m.base));
    }

    for (auto a : all)
        if (scanner::InExecutableSection(a)) return scanner::FollowJumps(a);
    return 0;
}

size_t Probe(const ModuleInfo& m) {
    memset(&g_api, 0, sizeof(g_api));
    size_t found = 0;
    for (const Sig& s : kSigs) {
        size_t hits = 0;
        const uintptr_t at = PickIn(m, s.pat, &hits);
        if (!at) continue;
        *s.slot = reinterpret_cast<void*>(at);
        ++found;
    }
    return found;
}

bool Resolve() {
    size_t best = 0;
    ModuleInfo bestMod{};
    uintptr_t probedBase = 0;

    for (auto& m : Ordered()) {
        if (!m.valid()) continue;
        const size_t score = Probe(m);
        probedBase = m.base;
        if (score > best) { best = score; bestMod = m; }
        if (score == kSigCount) break;
    }

    // Si el bucle no acabo en el ganador, g_api tiene las direcciones del
    // ultimo modulo probado: hay que re-probar el bueno.
    if (best && bestMod.valid() && probedBase != bestMod.base)
        Probe(bestMod);

    if (!g_api.ok()) {
        LogE("[lua] mejores firmas %zu/%zu en %s", best, kSigCount,
             bestMod.valid() ? bestMod.name.c_str() : "(ninguno)");
        return false;
    }

    g_target = reinterpret_cast<void*>(g_api.loadbufferx);
    Log("[lua] anfitrion %s, loadbufferx %p (%zu/%zu firmas)", bestMod.name.c_str(),
        g_target, best, kSigCount);
    return true;
}

int api_log(lua_State* L) {
    size_t len = 0;
    const char* text = g_api.tolstring(L, 1, &len);
    if (text) Log("[lua] %.*s", static_cast<int>(len), text);
    return 0;
}

int api_version(lua_State* L) { g_api.pushstring(L, "fivem-cheat 1.2"); return 1; }
int api_mouse1(lua_State* L) { g_api.pushinteger(L, GetAsyncKeyState(0x01) & 0x8000 ? 1 : 0); return 1; }

void RegisterInternal(lua_State* L) {
    const int top = g_api.gettop(L);

    if (g_api.loadbufferx(L, kRegChunk, strlen(kRegChunk), "@cheat_api", "t") != 0) {
        LogE("[lua] no se pudo compilar el chunk de registro");
        g_api.settop(L, top);
        return;
    }

    g_api.createtable(L, 0, 8);
    g_api.pushcclosure(L, api_log, 0);     g_api.setfield(L, -2, "log");
    g_api.pushcclosure(L, api_version, 0); g_api.setfield(L, -2, "version");
    g_api.pushcclosure(L, api_mouse1, 0);  g_api.setfield(L, -2, "mouse1");
    native::addLuaFields(L);

    if (g_api.pcallk(L, 1, 0, 0, 0, nullptr) != 0) {
        size_t len = 0;
        const char* err = g_api.tolstring(L, -1, &len);
        LogE("[lua] registro de la tabla cheat fallo: %.*s",
             static_cast<int>(len), err ? err : "?");
    }

    g_api.settop(L, top);
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
    g_registered = false;
    Log("runtime Lua listo%s", g_api.canRegister() ? "" : " (sin tabla cheat)");
    return true;
}

void shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_pending.clear();
    g_state = nullptr;
    g_ready = false;
    g_registered = false;
    g_target = nullptr;
}

bool ready() { return g_ready && g_state != nullptr; }
lua_State* state() { return g_state; }
void* target() { return g_target; }

int probeTop(lua_State* L) {
    if (!L || !g_api.gettop) return 0;
    return g_api.gettop(L);
}

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
        // Se refresca siempre: el estado que acaba de cargar un chunk esta vivo,
        // y RunString nunca trabaja sobre un L que ya murio.
        g_state = L;
        if (!g_registered) { g_registered = true; first = true; }
    }

    if (first) {
        Log("estado Lua capturado (%p)", L);
        RegisterApi(L);
        CallOnLuaStateSafe(L);
    }

    // Aqui SI estamos en el hilo del juego: es el unico sitio donde se puede
    // ejecutar Lua sin corromper la pila del runtime.
    if (PendingCount()) FlushPending();
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