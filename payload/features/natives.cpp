#include "features/natives.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------- hashes

// Todas opcionales: si una no resuelve, la feature que la usa se apaga sola en
// vez de llamar a la nativa equivocada.
enum : uint64_t {
    H_PLAYER_ID                        = 0x4F8644AF03D0E0D6,
    H_PLAYER_PED_ID                    = 0xD80958FC74E988A6,
    H_GET_PLAYER_PED                   = 0x43A66C31C68491C0,
    H_NETWORK_IS_PLAYER_ACTIVE         = 0xB8DFD30D6973E135,
    H_GET_ENTITY_COORDS                = 0x3FEF770D40960D5A,
    H_GET_ENTITY_HEALTH                = 0xEEF059FAD016D209,
    H_GET_ENTITY_HEADING               = 0xE83D4F9BA2A38914,
    H_GET_PED_BONE_COORDS              = 0x17C07FC640E86B4E,
    H_GET_SCREEN_COORD_FROM_WORLD      = 0x34E82F05DF2974F5,
    H_GET_PLAYER_NAME                  = 0x6D0DE6A7B5DA71F8,
    H_SET_PLAYER_INVINCIBLE            = 0x239528EACDC3E7DE,
    H_SET_ENTITY_INVINCIBLE            = 0x3882114BDE571AD4,
    H_SET_SUPER_JUMP_THIS_FRAME        = 0x57FFF03E423A4C0B,
    H_GET_GAMEPLAY_CAM_COORD           = 0x14D6F5678D8F1B37,
    H_SET_CAM_RELATIVE_HEADING         = 0xC2AB6BF4E8A9C5F7,
    H_SET_CAM_RELATIVE_PITCH           = 0x2F5C4B1C1C7E1A28,
};

// Hashes de validacion: si estos tres no resuelven, la tabla es falsa.
constexpr uint64_t kValidate[] = { H_PLAYER_ID, H_PLAYER_PED_ID, H_GET_ENTITY_COORDS };

// ---------------------------------------------------------------- layout

// rage::scrNativeRegistration: lista enlazada de bloques con hashes/handlers.
// La tabla global es un array de 256 punteros indexado por hash & 0xFF.
constexpr size_t kEntriesPerBlock = 7;
constexpr size_t kHashesOffset    = 0x08;
constexpr size_t kHandlersOffset  = 0x08 + kEntriesPerBlock * 8;
constexpr size_t kCountOffset     = kHandlersOffset + kEntriesPerBlock * 8;

using Handler_t = void (*)(void*);

// rage::scrNativeCallContext
struct NativeContext {
    void*    retn      = nullptr;
    uint32_t argCount  = 0;
    uint32_t pad0      = 0;
    void*    args      = nullptr;
    uint32_t dataCount = 0;
    uint32_t pad1      = 0;
    void*    vector    = nullptr;
};

void**   g_table  = nullptr;
bool     g_ready  = false;
const char* g_status = "sin resolver";

bool PlausibleExec(uintptr_t p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_EXECUTE;
}

// Recorre la tabla igual que el motor. Sin guard por fuera: el llamante mete
// esto en un __try.
void* Lookup(uint64_t hash) {
    if (!g_table) return nullptr;
    auto* block = reinterpret_cast<uint8_t*>(g_table[hash & 0xFF]);
    int guard = 0;
    while (block && guard++ < 512) {
        const auto count = *reinterpret_cast<uint32_t*>(block + kCountOffset);
        if (count > 0 && count <= kEntriesPerBlock) {
            auto* hashes = reinterpret_cast<uint64_t*>(block + kHashesOffset);
            auto* fns    = reinterpret_cast<void**>(block + kHandlersOffset);
            for (uint32_t i = 0; i < count; ++i)
                if (hashes[i] == hash && PlausibleExec(reinterpret_cast<uintptr_t>(fns[i])))
                    return fns[i];
        }
        block = *reinterpret_cast<uint8_t**>(block);
    }
    return nullptr;
}

// Extrae el destino de un `lea rcx, [rip+disp32]` (48 8D 0D) en la posicion.
uintptr_t LeaTarget(uintptr_t at) {
    const auto* b = reinterpret_cast<const uint8_t*>(at);
    int32_t disp = 0;
    memcpy(&disp, b + 3, sizeof(disp));
    return at + 7 + static_cast<intptr_t>(disp);
}

bool Validate(void** table) {
    g_table = table;
    for (uint64_t h : kValidate)
        if (!Lookup(h)) { g_table = nullptr; return false; }
    return true;
}

bool TryValidate(void** table) {
    __try {
        return Validate(table);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_table = nullptr;
        return false;
    }
}

// Candidatos al lea que referencia rage::scrNativeRegistrationTable. Se prueban
// todos y se queda el primero que valide con los hashes conocidos.
const char* kTablePatterns[] = {
    "48 8D 0D ? ? ? ? 48 8B D0 E8 ? ? ? ? 48 8B 5C 24 30",
    "48 8D 0D ? ? ? ? 48 8B D0 E8 ? ? ? ? 48 8B 5C 24 20",
    "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B D8 48 85 DB",
};

std::vector<ModuleInfo> Order() {
    std::vector<ModuleInfo> out;
    for (auto& m : modules::All()) {
        std::string l = m.name;
        for (auto& c : l) c = (char)::tolower((unsigned char)c);
        if (l.find("gta-core-five") != std::string::npos ||
            l.find("gtaprocess") != std::string::npos ||
            l.find("gta5") != std::string::npos)
            out.push_back(m);
    }
    return out;
}

bool Resolve() {
    for (auto& m : Order()) {
        for (const char* pat : kTablePatterns) {
            const auto hits = scanner::FindAll(m, pat, 8);
            for (auto hit : hits) {
                if (!scanner::InExecutableSection(hit)) continue;
                auto** table = reinterpret_cast<void**>(LeaTarget(hit));
                if (!table) continue;

                if (TryValidate(table)) {
                    Log("[natives] tabla en %s+0x%llX", m.name.c_str(),
                        static_cast<unsigned long long>(hit - m.base));
                    g_status = "resueltas";
                    return true;
                }
                Log("[natives] candidato descartado %s+0x%llX", m.name.c_str(),
                    static_cast<unsigned long long>(hit - m.base));
            }
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

bool has(uint64_t hash) {
    if (!g_ready) return false;
    __try { return Lookup(hash) != nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool call(uint64_t hash, void* ret, int argCount, const uint64_t* args) {
    if (!g_ready) return false;
    void* fn = nullptr;
    __try { fn = Lookup(hash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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