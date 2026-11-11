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

std::unordered_map<uint64_t, void*> g_map;   // cache / modo cosechado
bool        g_ready  = false;
bool        g_member = false;                // el resolver es miembro (this + hash)
const char* g_status = "sin resolver";

void* g_getHandler = nullptr;                // modo exportacion

struct Range { uintptr_t lo, hi; };
std::vector<Range> g_exec;

bool IsExec(uintptr_t p) {
    for (const auto& r : g_exec)
        if (p >= r.lo && p < r.hi) return true;
    return false;
}

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

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

// ------------------------------------------------------------------ modo 1

// Pregunta a la funcion de FiveM. Se prueban las dos convenciones: estatica
// (hash) y miembro (this, hash). La que devuelva algo ejecutable es la buena.
void* Ask(uint64_t hash) {
    if (!g_getHandler) return nullptr;
    void* h = nullptr;
    __try {
        if (g_member) {
            auto fn = reinterpret_cast<void*(*)(void*, uint64_t)>(g_getHandler);
            h = fn(nullptr, hash);
        } else {
            auto fn = reinterpret_cast<void*(*)(uint64_t)>(g_getHandler);
            h = fn(hash);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return IsExec(reinterpret_cast<uintptr_t>(h)) ? h : nullptr;
}

bool ProbeConvention() {
    __try {
        auto one = reinterpret_cast<void*(*)(uint64_t)>(g_getHandler);
        if (IsExec(reinterpret_cast<uintptr_t>(one(H_PLAYER_ID)))) { g_member = false; return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try {
        auto two = reinterpret_cast<void*(*)(void*, uint64_t)>(g_getHandler);
        if (IsExec(reinterpret_cast<uintptr_t>(two(nullptr, H_PLAYER_ID)))) { g_member = true; return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return false;
}

// Recorre la tabla de exportaciones del modulo buscando el resolver.
void* ScanExports(HMODULE h, const char* modName) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(h);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<uint8_t*>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.Size) return nullptr;

    auto* exp  = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(reinterpret_cast<uint8_t*>(h) + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfNames);
    auto* ords  = reinterpret_cast<WORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfNameOrdinals);
    auto* fns   = reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfFunctions);

    Log("[natives] %s: %lu exportaciones", modName, exp->NumberOfNames);

    void* best = nullptr;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(h) + names[i];
        const std::string l = Lower(n);

        if (l.find("nativehandler") == std::string::npos) continue;

        const DWORD rva = fns[ords[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;  // forwarder

        Log("[natives] candidato %s!%s", modName, n);

        if (l.find("getnativehandler") != std::string::npos) {
            best = reinterpret_cast<uint8_t*>(h) + rva;
            break;
        }
        if (!best) best = reinterpret_cast<uint8_t*>(h) + rva;
    }
    return best;
}

bool ResolveViaExports() {
    static const wchar_t* kMods[] = { L"gta-core-five.dll", L"scripting-gta.dll" };
    for (const wchar_t* w : kMods) {
        HMODULE h = GetModuleHandleW(w);
        if (!h) { Log("[natives] %ls no cargado", w); continue; }

        char name[64]{};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof(name) - 1, nullptr, nullptr);

        g_getHandler = ScanExports(h, name);
        if (!g_getHandler) continue;
        Log("[natives] resolver en %p", g_getHandler);

        if (ProbeConvention()) {
            for (uint64_t v : kValidate)
                if (!Ask(v)) { g_getHandler = nullptr; break; }
            if (g_getHandler) {
                g_status = g_member ? "resueltas (export, miembro)" : "resueltas (export)";
                return true;
            }
        }
        Log("[natives] el resolver de %s no respondio a los hashes conocidos", name);
        g_getHandler = nullptr;
    }
    return false;
}

// ------------------------------------------------------------------ modo 2

// Respaldo: cosecha de bloques con forma estricta (next, 7 hashes, 7 handlers,
// contador 1..7). Menos falsos positivos que mirar solo punteros.
size_t HarvestBlocks(const ModuleInfo& m) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(m.base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    size_t found = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;

        const uintptr_t lo  = m.base + sec->VirtualAddress;
        const uintptr_t end = lo + sec->Misc.VirtualSize;

        for (uintptr_t at = lo; at + 0x80 <= end; at += 8) {
            const auto* b = reinterpret_cast<const uint8_t*>(at);

            uintptr_t next = 0;
            memcpy(&next, b, 8);
            if (next && (next < lo || next >= end)) continue;

            uint32_t count = 0;
            memcpy(&count, b + 0x78, 4);
            if (count < 1 || count > 7) continue;

            bool ok = true;
            for (size_t k = 0; k < 7 && ok; ++k) {
                uint64_t hash = 0; void* fn = nullptr;
                memcpy(&hash, b + 0x08 + k * 8, 8);
                memcpy(&fn, b + 0x40 + k * 8, 8);
                if (!hash) { ok = false; break; }
                if (IsExec(static_cast<uintptr_t>(hash))) { ok = false; break; }
                if (!IsExec(reinterpret_cast<uintptr_t>(fn))) { ok = false; break; }
            }
            if (!ok) continue;

            for (size_t k = 0; k < 7; ++k) {
                uint64_t hash = 0; void* fn = nullptr;
                memcpy(&hash, b + 0x08 + k * 8, 8);
                memcpy(&fn, b + 0x40 + k * 8, 8);
                g_map[hash] = fn;
            }
            ++found;
            at += 0x78;
        }
    }
    return found;
}

void* Lookup(uint64_t hash) {
    if (g_getHandler) return Ask(hash);
    const auto it = g_map.find(hash);
    return it == g_map.end() ? nullptr : it->second;
}

bool Validate() {
    for (uint64_t h : kValidate)
        if (!Lookup(h)) return false;
    return true;
}

size_t TryHarvestBlocks(const ModuleInfo& m) {
    __try {
        return HarvestBlocks(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Resolve() {
    BuildExec();
    if (g_exec.empty()) { g_status = "sin rangos ejecutables"; return false; }

    if (ResolveViaExports()) {
        Log("[natives] %s", g_status);
        return true;
    }

    Log("[natives] exportaciones sin resultado, se prueba el cosechado");
    for (auto& m : modules::All()) {
        std::string l = Lower(m.name);
        if (l.find("gtaprocess") == std::string::npos && l.find("gta5") == std::string::npos) continue;

        const size_t n = TryHarvestBlocks(m);
        Log("[natives] %s: %zu bloques", m.name.c_str(), n);
        if (n && Validate()) {
            g_status = "resueltas (cosecha)";
            Log("[natives] cosechadas %zu nativas", g_map.size());
            return true;
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