#include "features/natives.h"
#include "utils/log.h"

#include <Windows.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    H_SET_CAM_RELATIVE_HEADING    = 0xB4EC2312F4E5B1F1,
    H_SET_CAM_RELATIVE_PITCH      = 0x6D0858B8EDFD2B7D,
    H_GET_ACTIVE_SCREEN_RES       = 0x873C9F3104101DD3,
};

using CallNative_t = bool (*)(uint64_t, void*);

CallNative_t g_callNative = nullptr;
int          g_layout     = 0;      // 1..4 segun cual valide
bool         g_ready      = false;
const char*  g_status     = "sin resolver";

std::unordered_set<uint64_t> g_missing;

// Buffer compatible con fx::ScriptContext / ScriptContextRaw / scrNativeCallContext.
// Soporta hasta 32 argumentos de 64 bits (MaxArguments = 32 en CitizenFX).
struct CtxBuf {
    alignas(16) uint8_t args[256]{};
    alignas(16) uint8_t results[64]{};
    alignas(16) uint8_t obj[64]{};
};

// Disposiciones probadas en fx::ScriptContext:
//   1) +0x00 args, +0x08 results, +0x10 nargs, +0x14 nresults (lee results)
//   2) +0x00 nargs, +0x04 nresults, +0x08 args, +0x10 results (lee results)
//   3) +0x00 m_argumentBuffer (shared args), +0x08 m_numArguments, +0x0C m_numResults (lee args)
//   4) +0x00 m_pReturn (results), +0x08 m_nArgCount, +0x10 m_pArgs (args), +0x18 m_nDataCount (lee results)
void FillCtx(CtxBuf& b, uint32_t nargs, uint32_t nresults) {
    memset(b.obj, 0, sizeof(b.obj));
    switch (g_layout) {
    case 1:
        *reinterpret_cast<uint8_t**>(b.obj + 0x00) = b.args;
        *reinterpret_cast<uint8_t**>(b.obj + 0x08) = b.results;
        *reinterpret_cast<uint32_t*>(b.obj + 0x10) = nargs;
        *reinterpret_cast<uint32_t*>(b.obj + 0x14) = nresults;
        break;
    case 2:
        *reinterpret_cast<uint32_t*>(b.obj + 0x00) = nargs;
        *reinterpret_cast<uint32_t*>(b.obj + 0x04) = nresults;
        *reinterpret_cast<uint8_t**>(b.obj + 0x08) = b.args;
        *reinterpret_cast<uint8_t**>(b.obj + 0x10) = b.results;
        break;
    case 3:
        *reinterpret_cast<uint8_t**>(b.obj + 0x00) = b.args;
        *reinterpret_cast<int32_t*>(b.obj + 0x08)  = static_cast<int32_t>(nargs);
        *reinterpret_cast<int32_t*>(b.obj + 0x0C)  = 0;
        break;
    default:
        *reinterpret_cast<uint8_t**>(b.obj + 0x00) = b.results;
        *reinterpret_cast<uint32_t*>(b.obj + 0x08) = nargs;
        *reinterpret_cast<uint8_t**>(b.obj + 0x10) = b.args;
        *reinterpret_cast<uint32_t*>(b.obj + 0x18) = 0;
        break;
    }
}

bool TryCallNative(uint64_t hash, void* ctxObj, bool& faulted) {
    faulted = false;
    __try {
        return g_callNative(hash, ctxObj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}

// Llama y devuelve false si el hash no existe o el contexto revienta.
bool Raw(uint64_t hash, uint8_t* out, uint32_t nargs, const uint64_t* args) {
    if (!g_callNative || nargs > 32) return false;
    if (g_ready && g_missing.find(hash) != g_missing.end()) return false;

    CtxBuf b;
    if (nargs && args) memcpy(b.args, args, static_cast<size_t>(nargs) * 8);
    FillCtx(b, nargs, 8);

    bool faulted = false;
    const bool ok = TryCallNative(hash, b.obj, faulted);
    if (!ok) {
        if (g_ready && !faulted) g_missing.insert(hash);
        return false;
    }
    if (out) {
        const uint8_t* src = (g_layout == 3) ? b.args : b.results;
        memcpy(out, src, 32);
    }
    return true;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

void* FindExport(HMODULE h, const char* modName, const char* want) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(h);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<uint8_t*>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.Size) return nullptr;

    auto* exp   = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(reinterpret_cast<uint8_t*>(h) + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfNames);
    auto* ords  = reinterpret_cast<WORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfNameOrdinals);
    auto* fns   = reinterpret_cast<DWORD*>(reinterpret_cast<uint8_t*>(h) + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(h) + names[i];
        if (Lower(n).find(want) == std::string::npos) continue;

        const DWORD rva = fns[ords[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;
        Log("[natives] %s!%s", modName, n);
        return reinterpret_cast<uint8_t*>(h) + rva;
    }
    return nullptr;
}

bool TryLayout(int layout) {
    g_layout = layout;
    int64_t id = -1;
    if (!Raw(H_PLAYER_ID, reinterpret_cast<uint8_t*>(&id), 0, nullptr)) return false;
    if (id < 0 || id > 31) return false;

    int64_t ped = 0;
    if (!Raw(H_PLAYER_PED_ID, reinterpret_cast<uint8_t*>(&ped), 0, nullptr)) return false;
    if (ped == 0) return false;

    Log("[natives] disposicion %d valida (player=%lld ped=%lld)", layout,
        static_cast<long long>(id), static_cast<long long>(ped));
    return true;
}

bool Resolve() {
    HMODULE h = GetModuleHandleW(L"scripting-gta.dll");
    if (!h) { g_status = "scripting-gta.dll no cargado"; return false; }

    auto* fn = FindExport(h, "scripting-gta.dll", "callnativehandler");
    if (!fn) { g_status = "sin CallNativeHandler"; return false; }

    g_callNative = reinterpret_cast<CallNative_t>(fn);
    Log("[natives] CallNativeHandler en %p", fn);

    for (int layout : { 1, 2, 3, 4 }) {
        if (TryLayout(layout)) {
            static const char* kLabels[] = {
                "", "resueltas (A)", "resueltas (B)", "resueltas (C)", "resueltas (D)"
            };
            g_status = kLabels[layout];
            return true;
        }
        Log("[natives] disposicion %d no cuadra", layout);
    }

    g_callNative = nullptr;
    g_status = "el contexto no valida";
    return false;
}

bool TryResolve() {
    __try {
        return Resolve();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogE("[natives] excepcion resolviendo");
        return false;
    }
}
} // namespace

namespace natives {

bool init() {
    if (g_ready) return true;
    static const char* s_lastLoggedStatus = nullptr;
    g_missing.clear();
    const bool ok = TryResolve();
    g_ready = ok;
    if (ok) {
        Log("[natives] %s", g_status);
        s_lastLoggedStatus = g_status;
    } else if (s_lastLoggedStatus != g_status) {
        LogE("[natives] %s", g_status);
        s_lastLoggedStatus = g_status;
    }
    return ok;
}

bool ready() { return g_ready; }
const char* statusText() { return g_status; }

bool has(uint64_t hash) {
    return g_ready && g_missing.find(hash) == g_missing.end();
}

bool call(uint64_t hash, void* ret, int argCount, const uint64_t* args) {
    if (!g_ready) return false;
    return Raw(hash, reinterpret_cast<uint8_t*>(ret), static_cast<uint32_t>(argCount), args);
}

void v_void(uint64_t hash, int argc, const uint64_t* args) {
    if (!g_ready) return;
    alignas(16) uint8_t ret[64]{};
    Raw(hash, ret, static_cast<uint32_t>(argc), args);
}

int64_t v_int(uint64_t hash, int argc, const uint64_t* args) {
    if (!g_ready) return 0;
    alignas(16) uint8_t ret[64]{};
    if (!Raw(hash, ret, static_cast<uint32_t>(argc), args)) return 0;
    return *reinterpret_cast<int64_t*>(ret);
}
} // namespace natives
