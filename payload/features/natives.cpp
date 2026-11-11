#include "features/natives.h"
#include "core/module.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

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

// Los handlers NO estan en el ejecutable: FiveM los reescribe a sus propios
// modulos y a bloques asignados. Hay que recoger las secciones ejecutables de
// TODOS los modulos, no solo del que contiene la tabla.
void BuildExec() {
    g_exec.clear();
    for (auto& m : modules::All()) {
        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(m.base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(m.base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const uintptr_t lo = m.base + sec->VirtualAddress;
            g_exec.push_back({ lo, lo + sec->Misc.VirtualSize });
        }
    }
}

// En un bloque de registro el hash de la entrada k esta exactamente `entries`
// posiciones antes de su handler. Asi que por cada puntero a codigo se mira
// `entries` atras y se empareja. No hace falta alinear bloques ni leer el
// contador, y un handler que FiveM haya dejado a 0 no rompe la deteccion.
size_t Harvest(const ModuleInfo& m, size_t entries) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(m.base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    size_t pairs = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        const uintptr_t lo = m.base + sec->VirtualAddress;
        const auto* p = reinterpret_cast<const uint64_t*>(lo);
        const size_t n = sec->Misc.VirtualSize / 8;

        for (size_t i = entries; i < n; ++i) {
            const uintptr_t fn = static_cast<uintptr_t>(p[i]);
            if (!IsExec(fn)) continue;
            const uint64_t h = p[i - entries];
            if (!h || IsExec(static_cast<uintptr_t>(h))) continue;
            g_map[h] = reinterpret_cast<void*>(fn);
            ++pairs;
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

std::vector<ModuleInfo> GameModules() {
    std::vector<ModuleInfo> out;
    for (auto& m : modules::All()) {
        std::string l = m.name;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.find("gtaprocess") != std::string::npos || l.find("gta5") != std::string::npos)
            out.push_back(m);
    }
    return out;
}

std::vector<ModuleInfo> FiveModules() {
    std::vector<ModuleInfo> out;
    for (auto& m : modules::All()) {
        std::string l = m.name;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.find("gta-core-five") != std::string::npos || l.find("scripting-gta") != std::string::npos)
            out.push_back(m);
    }
    return out;
}

bool TryHarvest(const std::vector<ModuleInfo>& mods, size_t entries, size_t* total) {
    *total = 0;
    for (auto& m : mods) {
        size_t n = 0;
        __try { n = Harvest(m, entries); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LogE("[natives] excepcion cosechando %s", m.name.c_str()); }
        *total += n;
        if (!n) continue;
        Log("[natives] %s: %zu pares con paso %zu", m.name.c_str(), n, entries);
        if (Validate()) {
            Log("[natives] tabla cosechada de %s (paso %zu, %zu nativas)",
                m.name.c_str(), entries, g_map.size());
            g_status = "resueltas";
            return true;
        }
    }
    return false;
}

bool Resolve() {
    BuildExec();
    if (g_exec.empty()) { g_status = "sin rangos ejecutables"; return false; }

    for (size_t entries : { size_t(7), size_t(8) }) {
        // Primero el ejecutable del juego (donde vive la tabla), luego los
        // modulos de FiveM por si el bloque se mudo de sitio.
        g_map.clear();
        size_t a = 0, b = 0;
        if (TryHarvest(GameModules(), entries, &a)) return true;
        if (TryHarvest(FiveModules(), entries, &b)) return true;
        Log("[natives] paso %zu sin validar (juego %zu, five %zu)", entries, a, b);
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