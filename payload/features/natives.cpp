#include "features/natives.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- hashes

enum : uint64_t {
    H_PLAYER_ID                   = 0x4F8644AF03D0E0D6,
    H_PLAYER_PED_ID               = 0xD80958FC74E988A6,
    H_GET_PLAYER_PED              = 0x43A66C31C68491C0,
    H_NETWORK_IS_PLAYER_ACTIVE    = 0xB8DFD30D6973E135,
    H_GET_ENTITY_COORDS           = 0x3FEF770D40960D5A,
    H_GET_ENTITY_HEALTH           = 0xEEF059FAD016D209,
    H_GET_PED_BONE_COORDS         = 0x17C07FC640E86B4E,
    H_GET_SCREEN_COORD_FROM_WORLD = 0x34E82F05DF2974F5,
    H_GET_PLAYER_NAME             = 0x6D0DE6A7B5DA71F8,
    H_SET_PLAYER_INVINCIBLE       = 0x239528EACDC3E7DE,
    H_SET_ENTITY_INVINCIBLE       = 0x3882114BDE571AD4,
    H_SET_SUPER_JUMP_THIS_FRAME   = 0x57FFF03E423A4C0B,
    H_GET_GAMEPLAY_CAM_COORD      = 0x14D6F5678D8F1B37,
    H_SET_CAM_RELATIVE_HEADING    = 0xC2AB6BF4E8A9C5F7,
    H_SET_CAM_RELATIVE_PITCH      = 0x2F5C4B1C1C7E1A28,
    H_GET_ACTIVE_SCREEN_RES       = 0x2E397A4B1C4B6F4F,
};

constexpr uint64_t kValidate[] = { H_PLAYER_ID, H_PLAYER_PED_ID, H_GET_ENTITY_COORDS };

// ---------------------------------------------------------------- contexto

using Handler_t = void (*)(void*);

struct NativeContext {
    void*    retn      = nullptr;
    uint32_t argCount  = 0;
    uint32_t pad0      = 0;
    void*    args      = nullptr;
    uint32_t dataCount = 0;
    uint32_t pad1      = 0;
    void*    vector    = nullptr;
};

std::unordered_map<uint64_t, void*> g_map;
bool        g_ready  = false;
const char* g_status = "sin resolver";

struct Range { uintptr_t lo, hi; };
std::vector<Range> g_exec;

bool IsExec(uintptr_t p) {
    for (const auto& r : g_exec)
        if (p >= r.lo && p < r.hi) return true;
    return false;
}

// ---------------------------------------------------------------- cosecha

// Recorre las secciones escribibles del modulo buscando rachas de punteros a
// codigo. Un bloque de registro es exactamente eso, y los kEntries qwords que
// hay JUSTO ANTES son sus hashes. No depende de ningun patron de bytes ni del
// offset del contador, asi que aguanta cambios de build.
size_t Harvest(const ModuleInfo& m, size_t entries) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(m.base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    std::vector<Range> data;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uintptr_t lo = m.base + sec->VirtualAddress;
        const uintptr_t hi = lo + sec->Misc.VirtualSize;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            g_exec.push_back({ lo, hi });
        } else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
            data.push_back({ lo, hi });
        }
    }
    if (g_exec.empty() || data.empty()) return 0;

    size_t pairs = 0;
    for (const auto& r : data) {
        const auto* p = reinterpret_cast<const uint64_t*>(r.lo);
        const size_t n = (r.hi - r.lo) / 8;
        size_t run = 0;

        for (size_t i = 0; i < n; ++i) {
            if (IsExec(static_cast<uintptr_t>(p[i]))) { ++run; continue; }

            if (run >= entries) {
                const size_t firstHandler = i - run;
                if (firstHandler >= entries) {
                    const uint64_t* hashes = p + firstHandler - entries;
                    const uint64_t* fns    = p + firstHandler;
                    for (size_t k = 0; k < entries; ++k) {
                        const uint64_t h = hashes[k];
                        if (h && IsExec(static_cast<uintptr_t>(fns[k])))
                            g_map[h] = reinterpret_cast<void*>(fns[k]);
                    }
                    pairs += entries;
                }
            }
            run = 0;
        }
    }
    return pairs;
}

void* Lookup(uint64_t hash) {
    const auto it = g_map.find(hash);
    return it == g_map.end() ? nullptr : it->second;
}

bool Validate() {
    for (uint64_t h : kValidate)
        if (!Lookup(h)) return false;
    return true;
}

size_t TryHarvest(const ModuleInfo& m, size_t entries, bool& faulted) {
    faulted = false;
    __try {
        return Harvest(m, entries);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

std::vector<ModuleInfo> Order() {
    std::vector<ModuleInfo> out;
    for (auto& m : modules::All()) {
        std::string l = m.name;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.find("gtaprocess") != std::string::npos || l.find("gta5") != std::string::npos)
            out.push_back(m);
    }
    for (auto& m : modules::All()) {
        std::string l = m.name;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.find("gta-core-five") != std::string::npos) out.push_back(m);
    }
    return out;
}

bool Resolve() {
    for (auto& m : Order()) {
        // El numero de entradas por bloque cambia entre builds: se prueban los
        // dos y se queda el que valide con hashes conocidos.
        for (size_t entries : { size_t(7), size_t(8) }) {
            g_map.clear();
            g_exec.clear();

            bool faulted = false;
            const size_t pairs = TryHarvest(m, entries, faulted);
            if (faulted) LogE("[natives] excepcion cosechando %s", m.name.c_str());
            if (!pairs) continue;

            Log("[natives] %s: %zu pares con bloques de %zu", m.name.c_str(), pairs, entries);
            if (Validate()) {
                Log("[natives] tabla cosechada de %s (bloques de %zu, %zu nativas)",
                    m.name.c_str(), entries, g_map.size());
                g_status = "resueltas";
                return true;
            }
            Log("[natives] %s: sin validar con bloques de %zu", m.name.c_str(), entries);
        }
    }
    g_status = "tabla de nativas no encontrada";
    return false;
}
} // namespace

namespace natives {

bool init() {
    if (g_ready) return true;
    bool ok = false;
    __try { ok = Resolve(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogE("[natives] excepcion resolviendo"); }
    g_ready = ok;
    if (!ok) LogE("[natives] %s", g_status);
    return ok;
}

bool ready() { return g_ready; }
const char* statusText() { return g_status; }

bool has(uint64_t hash) { return g_ready && Lookup(hash) != nullptr; }

bool call(uint64_t hash, void* ret, int argCount, const uint64_t* args) {
    if (!g_ready) return false;
    void* fn = Lookup(hash);
    if (!fn) return false;

    alignas(16) uint8_t vecData[48]{};
    NativeContext ctx;
    ctx.retn      = ret;
    ctx.argCount  = static_cast<uint32_t>(argCount);
    ctx.args      = const_cast<uint64_t*>(args);
    ctx.dataCount = 0;
    ctx.vector    = vecData;

    __try {
        reinterpret_cast<Handler_t>(fn)(&ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void v_void(uint64_t hash, int argc, const uint64_t* args) {
    alignas(16) uint8_t ret[64]{};
    call(hash, ret, argc, args);
}

int64_t v_int(uint64_t hash, int argc, const uint64_t* args) {
    alignas(16) uint8_t ret[64]{};
    if (!call(hash, ret, argc, args)) return 0;
    return *reinterpret_cast<int64_t*>(ret);
}
} // namespace natives