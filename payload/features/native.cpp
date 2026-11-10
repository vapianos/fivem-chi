#include "features/native.h"
#include "core/module.h"
#include "core/scanner.h"
#include "utils/log.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include "imgui.h"

namespace {

using Handler = void(*)(void*);
using Invoker = void(*)(void*, uint64_t);

struct Abi {
    int ret, argCount, dataCount, args, hash;
};

#pragma pack(push, 1)
struct Vec3 { float x, y, z; };

//   0x70 ret | 0x80..0xF0 argumentos
struct alignas(16) Ctx {
    uint8_t raw[0x100];
};
#pragma pack(pop)

enum Mode { MODE_TABLE, MODE_INVOKER };

Mode      g_mode  = MODE_TABLE;
void**    g_table = nullptr;
Invoker   g_invoker = nullptr;
int       g_hashOff    = 16;
int       g_handlerOff = 8;
Abi       g_abi{ 0x00, 0x08, 0x0C, 0x10, 0x18 };
bool      g_nativeOk  = false;
bool      g_hasScreen = false;
int       g_screenFails = 0;
unsigned  g_ticks = 0;

std::atomic<bool>  g_esp{ true }, g_box{ true }, g_aim{ false }, g_god{ false }, g_jump{ false };
std::atomic<bool>  g_skel{ true }, g_aimHold{ false };
std::atomic<bool>  g_scanStarted{ false };
std::atomic<float> g_fov{ 90.0f };
std::atomic<float> g_smooth{ 0.55f };

struct NameCache { int ped = 0; ULONGLONG at = 0; char name[32]{}; };
NameCache g_names[32];

constexpr float kDeg      = 0.01745329252f;
constexpr float kMaxDist  = 500.0f;
constexpr float kSkelDist = 300.0f;
constexpr float kAimDist  = 300.0f;

constexpr uint64_t N_HASH_KEY       = 0xD24D37CC275948CC;
constexpr uint64_t N_PLAYER_PED_ID  = 0xD80958FC74E988A6;
constexpr uint64_t N_PLAYER_ID      = 0x4F8644AF03D0E0D6;
constexpr uint64_t N_GET_PLAYER_PED = 0x43A66C31C68491C0;
constexpr uint64_t N_ACTIVE         = 0xB8DFD30D6973E135;
constexpr uint64_t N_COORDS         = 0x3FEF770D40960D5A;
constexpr uint64_t N_SCREEN         = 0x34E82F05DF2974F5;
constexpr uint64_t N_HEALTH         = 0xEEF059FAD016D209;
constexpr uint64_t N_INVINCIBLE     = 0xB7A628320EFF8E47;
constexpr uint64_t N_ENTITY_INV     = 0x8DC959DA5A9B4A6D;
constexpr uint64_t N_SUPERJUMP      = 0x8AA4643CD9E6D3B7;
constexpr uint64_t N_HEADING        = 0xE83D4F9BA2A38914;
constexpr uint64_t N_CAM_HEADING    = 0xDFC5209B5B4D0F2E;
constexpr uint64_t N_CAM_PITCH      = 0x5A4A6A6D;
constexpr uint64_t N_NAME           = 0x6D0DE6A7B5DA71F8;
constexpr uint64_t N_BONE           = 0x44A8FCB8ED227738;
constexpr uint64_t N_CAM_COORD      = 0x14D6F5678D8F1B37;
constexpr uint64_t N_CAM_ROT        = 0x837765A25378F0BB;
constexpr uint64_t N_CAM_FOV        = 0x65019750A0324133;

enum Bone : int {
    B_Head = 0, B_Neck = 1, B_Spine3 = 2, B_Spine2 = 3, B_Spine1 = 4, B_Spine0 = 5, B_Pelvis = 6,
    B_LClav = 7, B_RClav = 8,
    B_LUpperArm = 9, B_LForearm = 10, B_LHand = 11,
    B_RUpperArm = 12, B_RForearm = 13, B_RHand = 14,
    B_LThigh = 22, B_LCalf = 23, B_LFoot = 24,
    B_RThigh = 25, B_RCalf = 26, B_RFoot = 27,
    B_Count = 28,
};

struct BoneLink { int a, b; };
static const BoneLink kLinks[] = {
    { B_Head, B_Neck }, { B_Neck, B_Spine3 },
    { B_Spine3, B_Spine2 }, { B_Spine2, B_Spine1 }, { B_Spine1, B_Spine0 }, { B_Spine0, B_Pelvis },
    { B_Spine3, B_LClav }, { B_LClav, B_LUpperArm }, { B_LUpperArm, B_LForearm }, { B_LForearm, B_LHand },
    { B_Spine3, B_RClav }, { B_RClav, B_RUpperArm }, { B_RUpperArm, B_RForearm }, { B_RForearm, B_RHand },
    { B_Pelvis, B_LThigh }, { B_LThigh, B_LCalf }, { B_LCalf, B_LFoot },
    { B_Pelvis, B_RThigh }, { B_RThigh, B_RCalf }, { B_RCalf, B_RFoot },
};

const char* g_status = "sin iniciar";

char g_probeA[] = "abc";
char g_probeB[] = "venice";

uint64_t Read64(uintptr_t at) {
    uint64_t v = 0;
    memcpy(&v, reinterpret_cast<const void*>(at), sizeof(v));
    return v;
}

bool Readable(const void* p, size_t len) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + len <=
           reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool PlausiblePtr(uint64_t v) {
    return v > 0x10000 && v < 0x0000800000000000ULL;
}

bool HashLike(uint64_t v) {
    return v >= 0x0001000000000000ULL;
}

// joaat identico al del juego: GET_HASH_KEY lo aplica tras pasar a minusculas.
uint32_t Joaat(const char* s) {
    uint32_t h = 0;
    while (*s) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h += (uint8_t)c;
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

void Begin(Ctx& c, uint64_t hash) {
    memset(c.raw, 0, sizeof(c.raw));
    void* args = c.raw + 0x80;
    void* ret  = c.raw + 0x70;
    memcpy(c.raw + g_abi.args, &args, 8);
    memcpy(c.raw + g_abi.ret,  &ret,  8);
    memcpy(c.raw + g_abi.hash, &hash, 8);
}

void PushRaw(Ctx& c, void* v) {
    uint32_t n = 0;
    memcpy(&n, c.raw + g_abi.argCount, 4);
    void** args = nullptr;
    memcpy(&args, c.raw + g_abi.args, 8);
    if (!args || n >= 14) return;
    args[n] = v;
    ++n;
    memcpy(c.raw + g_abi.argCount, &n, 4);
}

void Push(Ctx& c, int v)         { PushRaw(c, (void*)(intptr_t)v); }
void Push(Ctx& c, bool v)        { PushRaw(c, (void*)(intptr_t)(v ? 1 : 0)); }
void Push(Ctx& c, float v)       { uint32_t b; memcpy(&b, &v, 4); PushRaw(c, (void*)(uintptr_t)b); }
void Push(Ctx& c, void* p)       { PushRaw(c, p); }
void Push(Ctx& c, float* p)      { PushRaw(c, p); }
void Push(Ctx& c, const char* p) { PushRaw(c, (void*)p); }
void Push(Ctx& c, Vec3* v) {
    PushRaw(c, v);
    uint32_t d = 0;
    memcpy(&d, c.raw + g_abi.dataCount, 4);
    ++d;
    memcpy(c.raw + g_abi.dataCount, &d, 4);
}

void* RetPtr(const Ctx& c) {
    void* p = nullptr;
    memcpy(&p, c.raw + g_abi.ret, 8);
    return p;
}

uint64_t Ret(const Ctx& c) {
    void* p = RetPtr(c);
    uint64_t v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}

Vec3 RetVec(const Ctx& c) {
    Vec3 v{};
    void* p = RetPtr(c);
    if (p && Readable(p, 12)) memcpy(&v, p, 12);
    return v;
}

float AsFloat(uint64_t raw) {
    float f = 0.0f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

struct HCache { uint64_t hash; Handler h; };
HCache g_hcache[256]{};

void ClearCache() { memset(g_hcache, 0, sizeof(g_hcache)); }

Handler Get(uint64_t hash) {
    if (!g_table) return nullptr;

    HCache& slot = g_hcache[hash & 0xFF];
    if (slot.h && slot.hash == hash) return slot.h;

    uintptr_t node = reinterpret_cast<uintptr_t>(g_table[hash & 0xFF]);
    for (int guard = 0; node && guard < 96; ++guard) {
        if (Read64(node + g_hashOff) == hash) {
            Handler h = reinterpret_cast<Handler>(Read64(node + g_handlerOff));
            if (!scanner::InExecutableSection(reinterpret_cast<uintptr_t>(h))) return nullptr;
            slot.hash = hash;
            slot.h = h;
            return h;
        }
        node = Read64(node);
    }
    return nullptr;
}

uint64_t Invoke(uint64_t hash, Ctx& c) {
    if (g_mode == MODE_INVOKER) {
        if (!g_invoker) return 0;
        g_invoker(c.raw, hash);
        return Ret(c);
    }
    Handler h = Get(hash);
    if (!h) return 0;
    h(c.raw);
    return Ret(c);
}

Vec3 InvokeVec(uint64_t hash, Ctx& c) {
    if (g_mode == MODE_INVOKER) {
        if (!g_invoker) return {};
        g_invoker(c.raw, hash);
        return RetVec(c);
    }
    Handler h = Get(hash);
    if (!h) return {};
    h(c.raw);
    return RetVec(c);
}

template <typename... A>
uint64_t Call(uint64_t hash, A... a) {
    Ctx c; Begin(c, hash);
    (Push(c, a), ...);
    return Invoke(hash, c);
}

Vec3 Coords(int ped) {
    Vec3 out{};
    if (!ped) return out;
    Ctx c; Begin(c, N_COORDS);
    Push(c, ped);
    Push(c, true);
    Push(c, &out);
    Invoke(N_COORDS, c);
    return out;
}

Vec3 Bone(int ped, int bone) {
    if (!ped) return {};
    Ctx c; Begin(c, N_BONE);
    Push(c, ped);
    Push(c, bone);
    return InvokeVec(N_BONE, c);
}

Vec3 CamCoord() {
    Vec3 out{};
    Ctx c; Begin(c, N_CAM_COORD);
    Push(c, &out);
    Invoke(N_CAM_COORD, c);
    return out;
}

Vec3 CamRot() {
    Vec3 out{};
    Ctx c; Begin(c, N_CAM_ROT);
    Push(c, 2);
    Push(c, &out);
    Invoke(N_CAM_ROT, c);
    return out;
}

float CamFov() {
    Ctx c; Begin(c, N_CAM_FOV);
    return AsFloat(Invoke(N_CAM_FOV, c));
}

bool W2SMatrix(const Vec3& w, float& sx, float& sy) {
    const Vec3 cam = CamCoord();
    const Vec3 rot = CamRot();
    const float fov = CamFov();
    if (fov < 1.0f || fov > 150.0f) return false;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.x < 16.0f || disp.y < 16.0f) return false;

    const float p = rot.x * kDeg, y = rot.z * kDeg;
    const float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy2 = sinf(y);

    const Vec3 fwd{ -sy2 * cp, cy * cp, -sp };
    const Vec3 right{ cy, sy2, 0.0f };
    const Vec3 up{ right.y * fwd.z - right.z * fwd.y,
                   right.z * fwd.x - right.x * fwd.z,
                   right.x * fwd.y - right.y * fwd.x };

    const Vec3 rel{ w.x - cam.x, w.y - cam.y, w.z - cam.z };
    const float z = rel.x * fwd.x + rel.y * fwd.y + rel.z * fwd.z;
    if (z <= 0.2f) return false;

    const float x  = rel.x * right.x + rel.y * right.y + rel.z * right.z;
    const float yy = rel.x * up.x + rel.y * up.y + rel.z * up.z;

    const float t = tanf(fov * 0.5f * kDeg);
    if (t <= 0.0001f) return false;

    const float aspect = disp.x / disp.y;
    const float cx = disp.x * 0.5f, cyy = disp.y * 0.5f;
    sx = cx + (x / z) * (1.0f / (t * aspect)) * cx;
    sy = cyy - (yy / z) * (1.0f / t) * cyy;
    return true;
}

bool W2S(const Vec3& w, float& sx, float& sy) {
    if (!g_nativeOk) return false;

    if (g_hasScreen) {
        float x = 0.0f, y = 0.0f;
        Ctx c; Begin(c, N_SCREEN);
        Push(c, w.x);
        Push(c, w.y);
        Push(c, w.z);
        Push(c, &x);
        Push(c, &y);
        if (Invoke(N_SCREEN, c)) {
            sx = x; sy = y;
            g_screenFails = 0;
            return true;
        }
        if (++g_screenFails > 60) {
            g_hasScreen = false;
            LogE("[native] native de pantalla sin respuesta, se pasa a matriz");
        }
    }
    return W2SMatrix(w, sx, sy);
}

// ------------------------------------------------------------------ candidatos

struct Candidate {
    Mode  mode;
    void* target;
    int   hashOff, handlerOff;
};

std::vector<Candidate> g_cands;

int ModuleScore(const std::string& name) {
    std::string l = name;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)::tolower(c); });
    if (l.find("citizen-scripting-core") != std::string::npos) return 0;
    if (l.find("gta-core-five") != std::string::npos)         return 1;
    if (l.find("citizen-scripting-lua") != std::string::npos) return 2;
    if (l.find("citizen") != std::string::npos)               return 3;
    if (l.find("scripting") != std::string::npos)             return 4;
    if (l.find("fivem") != std::string::npos)                 return 5;
    if (l.find("gtaprocess") != std::string::npos)            return 6;
    if (l.find("gta5") != std::string::npos)                  return 7;
    return 30;
}

std::vector<ModuleInfo> ScoredModules(int maxScore) {
    auto mods = modules::All();
    std::stable_sort(mods.begin(), mods.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        return ModuleScore(a.name) < ModuleScore(b.name);
    });
    std::vector<ModuleInfo> out;
    for (auto& m : mods) {
        if (ModuleScore(m.name) > maxScore) continue;
        if (m.size < 0x10000) continue;
        out.push_back(m);
    }
    return out;
}

struct InvPat { const char* text; size_t e8; };

void AddInvoker(uintptr_t fn, const char* where) {
    if (!fn || !scanner::InExecutableSection(fn)) return;
    for (auto& c : g_cands)
        if (c.mode == MODE_INVOKER && c.target == reinterpret_cast<void*>(fn)) return;
    if (g_cands.size() >= 24) return;
    g_cands.push_back({ MODE_INVOKER, reinterpret_cast<void*>(fn), 0, 0 });
    Log("[native] invocador candidato en %s (%p)", where, reinterpret_cast<void*>(fn));
}

void CollectInvokers() {
    static const InvPat kPats[] = {
        { "48 8D 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 8B CF", 10 },
        { "48 8D 0D ? ? ? ? 48 8B D7 E8", 10 },
        { "48 8B D7 E8 ? ? ? ? 48 8B CF", 3 },
        { "48 8D 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 85 C0", 10 },
    };

    for (auto& m : ScoredModules(4)) {
        if (g_cands.size() >= 12) break;

        size_t before = g_cands.size();
        for (const InvPat& p : kPats) {
            for (uintptr_t at : scanner::FindAll(m, p.text, 2)) {
                if (*reinterpret_cast<const uint8_t*>(at + p.e8) != 0xE8) continue;
                const int32_t rel = *reinterpret_cast<int32_t*>(at + p.e8 + 1);
                char where[96]{};
                snprintf(where, sizeof(where), "%s+0x%llX", m.name.c_str(),
                         (unsigned long long)(at - m.base));
                AddInvoker(at + p.e8 + 5 + rel, where);
            }
            if (g_cands.size() >= 12) break;
        }
        if (g_cands.size() != before)
            Log("[native] %zu invocador(es) en %s", g_cands.size() - before, m.name.c_str());
    }
    Log("[native] invocadores: %zu", g_cands.size());
}

// ---- tabla: se saca del codigo del invocador ------------------------------
//
// Aunque la convencion de llamada del invocador sea otra, su codigo es el
// correcto: dentro esta la referencia RIP-relativa al objeto NativeContext o a
// la tabla. Se leen esas referencias en vez de barrer memoria a ciegas.

bool AnalyzeTable(uintptr_t at, int& hashOff, int& handlerOff) {
    auto* t = reinterpret_cast<uint64_t*>(at);

    for (int i = 0; i < 8; ++i)
        if (!PlausiblePtr(t[i])) return false;

    int nonNull = 0;
    for (int i = 0; i < 256; i += 2) {
        const uint64_t n = t[i];
        if (!n) continue;
        if (!Readable(reinterpret_cast<void*>(n), 24)) return false;
        ++nonNull;
    }
    if (nonNull < 100) return false;

    int hits[2] = {};
    for (int i = 0; i < 256; i += 4) {
        uintptr_t n = static_cast<uintptr_t>(t[i]);
        for (int g = 0; g < 3 && n; ++g) {
            if (!Readable(reinterpret_cast<void*>(n), 24)) break;

            const uint64_t next = Read64(n);
            const uint64_t a = Read64(n + 8);
            const uint64_t b = Read64(n + 16);
            const bool execA = scanner::InExecutableSection(static_cast<uintptr_t>(a));
            const bool execB = scanner::InExecutableSection(static_cast<uintptr_t>(b));
            if (execA == execB) break;
            if (!HashLike(execA ? b : a)) break;

            ++hits[execA ? 0 : 1];
            n = static_cast<uintptr_t>(next);
        }
    }

    const int best = std::max(hits[0], hits[1]);
    if (best < 60) return false;

    hashOff = (hits[0] >= hits[1]) ? 16 : 8;
    handlerOff = (hits[0] >= hits[1]) ? 8 : 16;
    return true;
}

void AddTable(uintptr_t at, const char* where) {
    for (auto& c : g_cands)
        if (c.mode == MODE_TABLE && c.target == reinterpret_cast<void*>(at)) return;
    if (g_cands.size() >= 40) return;

    int h = 0, hd = 0;
    if (!AnalyzeTable(at, h, hd)) return;
    g_cands.push_back({ MODE_TABLE, reinterpret_cast<void*>(at), h, hd });
    Log("[native] tabla por %s (%p, hash+0x%X handler+0x%X)",
        where, reinterpret_cast<void*>(at), h, hd);
}

// Referencias RIP-relativas dentro del invocador: mov/lea r64, [rip+d]
void ScanInvokerCode(uintptr_t fn) {
    constexpr size_t kMax = 0x100;
    if (!Readable(reinterpret_cast<void*>(fn), kMax)) return;

    for (size_t i = 0; i + 7 < kMax; ++i) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(fn + i);
        const bool rex = (p[0] & 0xF8) == 0x48 || (p[0] & 0xF8) == 0x4C;
        if (!rex) continue;
        if (p[1] != 0x8B && p[1] != 0x8D) continue;
        if ((p[2] & 0xC7) != 0x05) continue;

        const int32_t rel = *reinterpret_cast<const int32_t*>(p + 3);
        const uintptr_t g = fn + i + 7 + rel;
        if (g < 0x10000) continue;

        if (p[1] == 0x8D) {
            AddTable(g, "lea del invocador");
        } else {
            AddTable(g, "mov del invocador");
            const uintptr_t v = static_cast<uintptr_t>(Read64(g));
            if (PlausiblePtr(v)) AddTable(v, "puntero del invocador");
        }
    }
}

void CollectTablesFromInvokers() {
    for (auto& c : g_cands)
        if (c.mode == MODE_INVOKER) ScanInvokerCode(reinterpret_cast<uintptr_t>(c.target));
    Log("[native] tablas desde invocadores: %zu",
        static_cast<size_t>(std::count_if(g_cands.begin(), g_cands.end(),
            [](const Candidate& c) { return c.mode == MODE_TABLE; })));
}

void ScanDataRegion(uintptr_t from, uintptr_t to, const char* where, int& found) {
    for (uintptr_t at = (from + 7) & ~(uintptr_t)7; at + 256 * 8 <= to; at += 8) {
        int h = 0, hd = 0;
        if (!AnalyzeTable(at, h, hd)) continue;
        g_cands.push_back({ MODE_TABLE, reinterpret_cast<void*>(at), h, hd });
        Log("[native] tabla en %s (%p, hash+0x%X handler+0x%X)",
            where, reinterpret_cast<void*>(at), h, hd);
        if (++found >= 12) return;
    }
}

void CollectTablesBySweep() {
    int found = 0;

    for (auto& m : ScoredModules(7)) {
        if (found >= 12) break;
        uintptr_t cursor = m.base;
        const uintptr_t end = m.base + m.size;

        while (cursor < end && found < 12) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi))) break;

            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t re = rb + mbi.RegionSize;
            if (re > end) re = end;

            const DWORD p = mbi.Protect;
            const bool data = mbi.State == MEM_COMMIT && !(p & (PAGE_GUARD | PAGE_NOACCESS)) &&
                              (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_READONLY)) &&
                              !(p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

            if (data && re > cursor + 256 * 8) ScanDataRegion(cursor, re, m.name.c_str(), found);
            cursor = re ? re : cursor + 0x1000;
        }
    }
    Log("[native] tablas por barrido: %d", found);
}

// -------------------------------------------------------------- comprobacion

const Abi kAbis[] = {
    { 0x00, 0x08, 0x0C, 0x10, 0x18 },
    { 0x00, 0x08, 0x0C, 0x18, 0x10 },
    { 0x08, 0x10, 0x14, 0x18, 0x20 },
    { 0x08, 0x10, 0x14, 0x20, 0x18 },
    { 0x00, 0x10, 0x14, 0x18, 0x20 },
};

void Apply(const Candidate& c) {
    g_mode = c.mode;
    g_table = (c.mode == MODE_TABLE) ? reinterpret_cast<void**>(c.target) : nullptr;
    g_invoker = (c.mode == MODE_INVOKER) ? reinterpret_cast<Invoker>(c.target) : nullptr;
    g_hashOff = c.hashOff;
    g_handlerOff = c.handlerOff;
    ClearCache();
}

void Accept(const Candidate& c) {
    g_nativeOk = true;
    g_hasScreen = (g_mode == MODE_TABLE) ? (Get(N_SCREEN) != nullptr) : false;
    g_screenFails = 0;
    g_status = (g_mode == MODE_INVOKER) ? "listo (invocador)" : "listo (tabla)";
    Log("[native] VALIDADO con %s en %p", g_mode == MODE_INVOKER ? "invocador" : "tabla", c.target);
}

bool Confirm(const Candidate& c) {
    Apply(c);

    for (const Abi& a : kAbis) {
        g_abi = a;

        uint32_t got = 0;
        __try { got = static_cast<uint32_t>(Call(N_HASH_KEY, g_probeA)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (got != Joaat(g_probeA)) continue;

        uint32_t got2 = 0;
        __try { got2 = static_cast<uint32_t>(Call(N_HASH_KEY, g_probeB)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (got2 != Joaat(g_probeB)) continue;

        Accept(c);
        return true;
    }
    return false;
}

void TryResolve() {
    g_status = "probando invocadores";
    CollectInvokers();
    for (size_t i = 0; i < g_cands.size(); ++i)
        if (Confirm(g_cands[i])) return;

    g_status = "buscando tabla";
    CollectTablesFromInvokers();
    for (size_t i = 0; i < g_cands.size(); ++i) {
        if (g_cands[i].mode != MODE_TABLE) continue;
        if (Confirm(g_cands[i])) return;
    }

    g_status = "barriendo datos";
    CollectTablesBySweep();
    for (size_t i = 0; i < g_cands.size(); ++i) {
        if (g_cands[i].mode != MODE_TABLE) continue;
        if (Confirm(g_cands[i])) return;
    }

    Log("[native] sin resolucion (%zu candidatos)", g_cands.size());
    g_cands.clear();
}

DWORD WINAPI ScanThread(LPVOID) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (g_nativeOk) return 0;

        __try {
            TryResolve();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogE("[native] excepcion durante el escaneo");
        }

        if (g_nativeOk) return 0;
        g_status = "sin candidato valido, reintentando";
        Sleep(2500);
    }
    return 0;
}
} // namespace

namespace native {

bool ready() { return g_nativeOk; }
bool usingMatrix() { return !g_hasScreen; }
const char* statusText() { return g_status; }

bool esp()     { return g_esp; }
bool box()     { return g_box; }
bool aim()     { return g_aim; }
bool god()     { return g_god; }
bool jump()    { return g_jump; }
bool skel()    { return g_skel; }
bool aimHold() { return g_aimHold; }
float fov()    { return g_fov; }
float smooth() { return g_smooth; }

void setEsp(bool v)     { g_esp = v; }
void setBox(bool v)     { g_box = v; }
void setAim(bool v)     { g_aim = v; }
void setGod(bool v)     { g_god = v; }
void setJump(bool v)    { g_jump = v; }
void setSkel(bool v)    { g_skel = v; }
void setAimHold(bool v) { g_aimHold = v; }
void setFov(float v)    { g_fov = v; }
void setSmooth(float v) { g_smooth = v; }

void rearm() {
    if (g_nativeOk) return;
    g_status = "rearme manual";
    Log("rearme de resolucion solicitado");
}

bool init() {
    if (g_nativeOk) return true;
    if (!g_scanStarted.exchange(true)) {
        g_status = "escaneando";
        CloseHandle(CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr));
    }
    return g_nativeOk;
}

void shutdown() {
    g_table = nullptr;
    g_invoker = nullptr;
    g_nativeOk = false;
    ClearCache();
}

void tick() {
    if (!ready()) return;
    ++g_ticks;

    if (!g_god && !g_jump && !g_aim) return;

    __try {
        const int me = static_cast<int>(Call(N_PLAYER_ID));
        if (me < 0 || me > 32) return;

        const int selfPed = static_cast<int>(Call(N_PLAYER_PED_ID));

        if (g_god) {
            Call(N_INVINCIBLE, me, true);
            if (selfPed) Call(N_ENTITY_INV, selfPed, true);
        }
        if (g_jump) Call(N_SUPERJUMP, me);

        if (!g_aim || !selfPed) return;
        if (g_aimHold && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;

        const Vec3 self = Coords(selfPed);
        if (self.x == 0.0f && self.y == 0.0f) return;

        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x < 16.0f) return;
        const float cx = disp.x * 0.5f, cy = disp.y * 0.5f;

        int best = 0;
        float bestD = g_fov;
        Vec3 bestPos{};

        for (int i = 0; i < 32; ++i) {
            if (i == me || !Call(N_ACTIVE, i)) continue;
            const int ped = static_cast<int>(Call(N_GET_PLAYER_PED, i));
            if (!ped) continue;

            const Vec3 pos = Coords(ped);
            const float dxw = pos.x - self.x, dyw = pos.y - self.y, dzw = pos.z - self.z;
            if (dxw * dxw + dyw * dyw + dzw * dzw > kAimDist * kAimDist) continue;

            float sx, sy;
            if (!W2S(pos, sx, sy)) continue;
            const float dx = sx - cx, dy = sy - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d < bestD) { bestD = d; best = ped; bestPos = pos; }
        }
        if (!best) return;

        const float dx = bestPos.x - self.x, dy = bestPos.y - self.y, dz = bestPos.z - self.z;
        const float horiz = sqrtf(dx * dx + dy * dy);
        if (horiz < 0.1f) return;

        const float bearing = atan2f(dx, dy) * 57.29578f;
        const float heading = AsFloat(Call(N_HEADING, selfPed));
        const float pitch = -atan2f(dz, horiz) * 57.29578f;

        static float s_relH = 0.0f, s_relP = 0.0f;
        static int   s_lastTarget = 0;

        float relH = bearing - heading;
        while (relH > 180.0f)  relH -= 360.0f;
        while (relH < -180.0f) relH += 360.0f;

        if (best != s_lastTarget || fabsf(relH - s_relH) > 60.0f) {
            s_relH = relH;
            s_relP = pitch;
            s_lastTarget = best;
        } else {
            const float k = g_smooth;
            s_relH += (relH - s_relH) * k;
            s_relP += (pitch - s_relP) * k;
        }

        Call(N_CAM_HEADING, s_relH);
        Call(N_CAM_PITCH, s_relP, 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogE("[native] excepcion en tick, frame saltado");
    }
}

const char* NameOf(int i, int ped, ULONGLONG now) {
    NameCache& c = g_names[i & 31];
    if (c.ped == ped && (now - c.at) <= 2000 && c.name[0]) return c.name;

    c.ped = ped;
    c.at = now;
    c.name[0] = '\0';

    const char* raw = reinterpret_cast<const char*>(Call(N_NAME, i));
    if (raw && Readable(raw, 32)) {
        memcpy(c.name, raw, 31);
        c.name[31] = '\0';
    }
    return c.name;
}

void DrawSkeleton(ImDrawList* dl, int ped) {
    static float sx[B_Count], sy[B_Count];
    static bool  ok[B_Count];
    memset(ok, 0, sizeof(ok));

    for (const BoneLink& l : kLinks) {
        const int need[2] = { l.a, l.b };
        for (int k = 0; k < 2; ++k) {
            const int b = need[k];
            if (ok[b]) continue;
            const Vec3 w = Bone(ped, b);
            if (w.x == 0.0f && w.y == 0.0f && w.z == 0.0f) continue;
            ok[b] = W2S(w, sx[b], sy[b]);
        }
    }

    for (const BoneLink& l : kLinks) {
        if (!ok[l.a] || !ok[l.b]) continue;
        dl->AddLine(ImVec2(sx[l.a], sy[l.a]), ImVec2(sx[l.b], sy[l.b]),
                    IM_COL32(255, 255, 255, 190), 1.2f);
    }
}

void render() {
    if (!ready() || (!g_esp && !g_box && !g_skel)) return;
    if (!ImGui::GetCurrentContext()) return;
    if (g_ticks & 1) return;

    __try {
        const int me = static_cast<int>(Call(N_PLAYER_ID));
        const int selfPed = static_cast<int>(Call(N_PLAYER_PED_ID));
        if (!selfPed) return;
        const Vec3 self = Coords(selfPed);

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        const ULONGLONG now = GetTickCount64();

        for (int i = 0; i < 32; ++i) {
            if (i == me || !Call(N_ACTIVE, i)) continue;
            const int ped = static_cast<int>(Call(N_GET_PLAYER_PED, i));
            if (!ped) continue;

            const Vec3 feet = Coords(ped);
            const float ddx = feet.x - self.x, ddy = feet.y - self.y, ddz = feet.z - self.z;
            const float dist = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
            if (dist > kMaxDist) continue;

            Vec3 head = Bone(ped, B_Head);
            const bool realHead = !(head.x == 0.0f && head.y == 0.0f && head.z == 0.0f);
            if (!realHead) { head = feet; head.z += 1.85f; }

            float fx, fy, hx, hy;
            if (!W2S(feet, fx, fy)) continue;
            if (!W2S(head, hx, hy)) continue;

            float boxH = fabsf(fy - hy);
            if (boxH < 8.0f) boxH = 8.0f;
            if (boxH > 900.0f) boxH = 900.0f;
            const float boxW = boxH * 0.42f;

            if (g_box) {
                dl->AddRect(ImVec2(hx - boxW * 0.5f, hy), ImVec2(hx + boxW * 0.5f, fy),
                            IM_COL32(0, 255, 128, 200), 0.0f, 0, 1.5f);
            }

            if (g_skel && dist <= kSkelDist) DrawSkeleton(dl, ped);

            if (g_esp) {
                const char* name = NameOf(i, ped, now);
                const int hp = static_cast<int>(Call(N_HEALTH, ped));
                char label[160]{};
                snprintf(label, sizeof(label), "%s\n%d hp\n%.0f m",
                         (name && name[0]) ? name : "jugador", hp, dist);
                dl->AddText(ImVec2(hx + boxW * 0.55f, hy), IM_COL32(0, 255, 128, 255), label);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogE("[native] excepcion en render, frame saltado");
    }
}

void renderMenu() {
    if (!ready()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "natives: %s", g_status);
        ImGui::Separator();
    }

    bool e = g_esp, b = g_box, a = g_aim, g = g_god, j = g_jump, s = g_skel, h = g_aimHold;
    float f = g_fov, sm = g_smooth;

    if (ImGui::Checkbox("ESP", &e))        g_esp = e;
    if (ImGui::Checkbox("Cajas", &b))      g_box = b;
    if (ImGui::Checkbox("Esqueleto", &s))  g_skel = s;
    if (ImGui::Checkbox("Aimbot", &a))     g_aim = a;
    if (ImGui::Checkbox("Aim solo con click derecho", &h)) g_aimHold = h;
    if (ImGui::SliderFloat("FOV aim", &f, 10.0f, 400.0f, "%.0f px")) g_fov = f;
    if (ImGui::SliderFloat("Suavizado", &sm, 0.05f, 1.0f, "%.2f")) g_smooth = sm;

    ImGui::Separator();

    if (ImGui::Checkbox("Godmode", &g))    g_god = g;
    if (ImGui::Checkbox("Superjump", &j))  g_jump = j;

    ImGui::Separator();
    ImGui::Text("proyeccion: %s", g_hasScreen ? "native" : "matriz");
    ImGui::Text("modo: %s", g_mode == MODE_INVOKER ? "invocador" : "tabla");
    ImGui::Text("alcance: %.0f m", kMaxDist);

    if (!g_nativeOk) {
        ImGui::Separator();
        if (ImGui::Button("Reintentar resolucion de natives")) rearm();
    }
}
} // namespace native