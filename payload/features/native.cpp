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

using Handler  = void(*)(void*);
using Invoker  = void(*)(void*, uint64_t);

struct Reg {
    Reg*     next;
    Handler  handler;
    uint64_t hash;
};

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

Mode      g_mode   = MODE_TABLE;
Reg**     g_table  = nullptr;
Invoker   g_invoker = nullptr;
uintptr_t g_game   = 0;
size_t    g_size   = 0;
int       g_hashOff    = 16;
int       g_handlerOff = 8;
Abi       g_abi{ 0x00, 0x08, 0x0C, 0x10, 0x18 };
bool      g_nativeOk  = false;
bool      g_hasScreen = false;
int       g_screenFails = 0;
unsigned  g_ticks = 0;

std::atomic<bool>  g_esp{ true }, g_box{ true }, g_aim{ false }, g_god{ false }, g_jump{ false };
std::atomic<float> g_fov{ 90.0f };

// Nombre cacheado: GET_PLAYER_NAME reserva en el heap del juego.
struct NameCache { int ped = 0; ULONGLONG at = 0; char name[32]{}; };
NameCache g_names[32];

constexpr float kDeg = 0.01745329252f;

constexpr uint64_t N_PLAYER_PED_ID  = 0xD80958FC74E988A6;
constexpr uint64_t N_PLAYER_ID      = 0x4F8644AF03D0E0D6;
constexpr uint64_t N_GET_PLAYER_PED = 0x43A66C31C68491C0;
constexpr uint64_t N_ACTIVE         = 0xB8DFD30D6973E135;
constexpr uint64_t N_COORDS         = 0x3FEF770D40960D5A;
constexpr uint64_t N_SCREEN         = 0x34E82F05DF2974F5;
constexpr uint64_t N_HEALTH         = 0xEEF059FAD016D209;
constexpr uint64_t N_INVINCIBLE     = 0xB7A628320EFF8E47;
constexpr uint64_t N_SUPERJUMP      = 0x8AA4643CD9E6D3B7;
constexpr uint64_t N_HEADING        = 0xE83D4F9BA2A38914;
constexpr uint64_t N_CAM_HEADING    = 0xDFC5209B5B4D0F2E;
constexpr uint64_t N_CAM_PITCH      = 0x5A4A6A6D;
constexpr uint64_t N_NAME           = 0x6D0DE6A7B5DA71F8;
constexpr uint64_t N_CAM_COORD      = 0x14D6F5678D8F1B37;
constexpr uint64_t N_CAM_ROT        = 0x837765A25378F0BB;
constexpr uint64_t N_CAM_FOV        = 0x65019750A0324133;

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

void Push(Ctx& c, int v)    { PushRaw(c, (void*)(intptr_t)v); }
void Push(Ctx& c, bool v)   { PushRaw(c, (void*)(intptr_t)(v ? 1 : 0)); }
void Push(Ctx& c, float v)  { uint32_t b; memcpy(&b, &v, 4); PushRaw(c, (void*)(uintptr_t)b); }
void Push(Ctx& c, void* p)  { PushRaw(c, p); }
void Push(Ctx& c, float* p) { PushRaw(c, p); }
void Push(Ctx& c, Vec3* v) {
    PushRaw(c, v);
    uint32_t d = 0;
    memcpy(&d, c.raw + g_abi.dataCount, 4);
    ++d;
    memcpy(c.raw + g_abi.dataCount, &d, 4);
}

uint64_t Ret(const Ctx& c) {
    void* p = nullptr;
    memcpy(&p, c.raw + g_abi.ret, 8);
    uint64_t v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}

float AsFloat(uint64_t raw) {
    float f = 0.0f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

Handler Get(uint64_t hash) {
    if (!g_table) return nullptr;
    uintptr_t node = reinterpret_cast<uintptr_t>(g_table[hash & 0xFF]);
    for (int guard = 0; node && guard < 64; ++guard) {
        if (Read64(node + g_hashOff) == hash)
            return reinterpret_cast<Handler>(Read64(node + g_handlerOff));
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

constexpr uint64_t kProbes[] = {
    N_PLAYER_PED_ID, N_PLAYER_ID, N_COORDS, N_HEALTH,
    N_INVINCIBLE, N_SUPERJUMP, N_NAME, N_CAM_COORD,
};

int ScoreTable(Reg** t, int hashOff, int handlerOff) {
    if (reinterpret_cast<uintptr_t>(t) < 0x10000) return 0;
    if (!Readable(t, 8)) return 0;

    int score = 0;
    __try {
        for (uint64_t probe : kProbes) {
            uintptr_t node = reinterpret_cast<uintptr_t>(t[probe & 0xFF]);
            for (int guard = 0; node && guard < 64; ++guard) {
                if (Read64(node + hashOff) == probe) {
                    if (scanner::InExecutableSection(static_cast<uintptr_t>(Read64(node + handlerOff))))
                        ++score;
                    break;
                }
                node = Read64(node);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return score;
}

const Abi kAbis[] = {
    { 0x00, 0x08, 0x0C, 0x10, 0x18 },
    { 0x00, 0x08, 0x0C, 0x18, 0x10 },
    { 0x08, 0x10, 0x14, 0x18, 0x20 },
    { 0x08, 0x10, 0x14, 0x20, 0x18 },
    { 0x00, 0x10, 0x14, 0x18, 0x20 },
    { 0x00, 0x08, 0x0C, 0x20, 0x18 },
};

// Solo natives sin reservas: PLAYER_ID, PLAYER_PED_ID y coords.
bool TestAbi(const Abi& a) {
    g_abi = a;

    int me = -1;
    __try {
        me = static_cast<int>(Call(N_PLAYER_ID));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (me < 0 || me > 32) return false;

    int ped = 0;
    __try {
        ped = static_cast<int>(Call(N_GET_PLAYER_PED, me));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (ped <= 0) return false;

    Vec3 p{};
    __try {
        Call(N_COORDS, ped, true, &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return fabsf(p.x) > 0.5f || fabsf(p.y) > 0.5f;
}

bool DetectAbi(const char* what, int maxAttempts) {
    int tries = 0;
    for (const Abi& a : kAbis) {
        if (tries++ >= maxAttempts) break;
        if (!TestAbi(a)) continue;
        Log("[native] contexto valido con %s (ret+0x%X args+0x%X)", what, a.ret, a.args);
        return true;
    }
    return false;
}

uintptr_t FunctionStart(uintptr_t hit) {
    static const uint8_t kProl[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

    for (int back = 1; back < 0x200; ++back) {
        uintptr_t p = hit - back;
        const uint8_t here = *reinterpret_cast<const uint8_t*>(p);
        const uint8_t prev = *reinterpret_cast<const uint8_t*>(p - 1);

        if (here == 0xCC && prev == 0xCC) return p + 1;
        if (prev == 0xCC || prev == 0xC3) {
            if (!memcmp(reinterpret_cast<const void*>(p), kProl, sizeof(kProl))) return p;
            if (here == 0x40 && *reinterpret_cast<const uint8_t*>(p + 1) == 0x55) return p;
            if (here == 0x48 && *reinterpret_cast<const uint8_t*>(p + 1) == 0x83 &&
                *reinterpret_cast<const uint8_t*>(p + 2) == 0xEC) return p;
        }
    }
    return 0;
}

uintptr_t FindGame() {
    ModuleInfo m = modules::Game();
    if (!m.valid()) {
        LogE("[native] no se encontro el modulo del juego");
        return 0;
    }
    g_size = m.size;
    return m.base;
}

bool Resolve() {
    g_game = FindGame();
    if (!g_game) return false;

    ModuleInfo mod{};
    mod.base = g_game;
    mod.size = g_size;

    // Solo los patrones especificos para el invocador: los genericos dan 128
    // candidatas y cada intento equivocado reserva en el heap del juego.
    static const char* kSpecific[] = {
        "48 8D 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 8B CF",
        "48 8D 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 85 C0",
        "48 8B 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 8B CF",
    };

    std::vector<uintptr_t> starts;
    for (const char* pat : kSpecific) {
        auto hits = scanner::FindAll(mod, pat, 8);
        Log("[native] patron '%s' -> %zu coincidencias", pat, hits.size());
        for (uintptr_t at : hits) {
            uintptr_t start = FunctionStart(at);
            if (!start || !scanner::InExecutableSection(start)) continue;
            if (std::find(starts.begin(), starts.end(), start) != starts.end()) continue;
            starts.push_back(start);
        }
    }

    Log("[native] %zu funciones candidatas a invocador", starts.size());
    for (uintptr_t start : starts) {
        Log("[native] probando invocador %p", reinterpret_cast<void*>(start));
        Sleep(40);
        g_mode = MODE_INVOKER;
        g_invoker = reinterpret_cast<Invoker>(start);
        if (DetectAbi("invocador", 3)) {
            Log("[native] invocador aceptado en %p", reinterpret_cast<void*>(start));
            return true;
        }
        Log("[native] descartado %p", reinterpret_cast<void*>(start));
    }

    // Respaldo por tabla, solo con los mismos patrones especificos.
    g_invoker = nullptr;
    g_mode = MODE_TABLE;

    Reg** best = nullptr;
    int   bestScore = 0;
    int   bestHash = g_hashOff, bestHandler = g_handlerOff;

    for (const char* pat : kSpecific) {
        auto hits = scanner::FindAll(mod, pat, 8);
        for (uintptr_t at : hits) {
            const int32_t rel = *reinterpret_cast<int32_t*>(at + 3);
            auto* slot = reinterpret_cast<Reg***>(at + 7 + rel);
            const uintptr_t raw = reinterpret_cast<uintptr_t>(slot);
            if (raw < 0x10000) continue;

            for (int pass = 0; pass < 2; ++pass) {
                Reg** candidate = pass ? reinterpret_cast<Reg**>(Read64(raw))
                                       : reinterpret_cast<Reg**>(raw);
                if (reinterpret_cast<uintptr_t>(candidate) < 0x10000) continue;

                const int sA = ScoreTable(candidate, 16, 8);
                if (sA > bestScore) { bestScore = sA; best = candidate; bestHash = 16; bestHandler = 8; }

                const int sB = ScoreTable(candidate, 8, 16);
                if (sB > bestScore) { bestScore = sB; best = candidate; bestHash = 8; bestHandler = 16; }
            }
        }
    }

    if (!best || bestScore < 3) {
        LogE("[native] ni invocador ni tabla (mejor puntuacion %d)", bestScore);
        return false;
    }

    g_table      = best;
    g_hashOff    = bestHash;
    g_handlerOff = bestHandler;
    Log("[native] tabla en %p (puntuacion %d/%zu)", static_cast<void*>(g_table),
        bestScore, sizeof(kProbes) / sizeof(kProbes[0]));
    return DetectAbi("tabla", 6);
}
} // namespace

namespace native {

bool ready() { return g_nativeOk; }
bool usingMatrix() { return !g_hasScreen; }

bool esp()  { return g_esp; }
bool box()  { return g_box; }
bool aim()  { return g_aim; }
bool god()  { return g_god; }
bool jump() { return g_jump; }
float fov() { return g_fov; }

void setEsp(bool v)  { g_esp = v; }
void setBox(bool v)  { g_box = v; }
void setAim(bool v)  { g_aim = v; }
void setGod(bool v)  { g_god = v; }
void setJump(bool v) { g_jump = v; }
void setFov(float v) { g_fov = v; }

bool init() {
    if (g_nativeOk) return true;
    if (!Resolve()) {
        g_table = nullptr;
        g_invoker = nullptr;
        return false;
    }

    g_hasScreen = (g_mode == MODE_TABLE) ? (Get(N_SCREEN) != nullptr) : false;
    g_nativeOk = true;
    Log("[native] listo (modo %s, pantalla %s)", g_mode == MODE_INVOKER ? "invocador" : "tabla",
        g_hasScreen ? "native" : "matriz");
    return true;
}

void shutdown() {
    g_table = nullptr;
    g_invoker = nullptr;
    g_nativeOk = false;
}

void tick() {
    if (!ready()) return;
    ++g_ticks;

    const int me = static_cast<int>(Call(N_PLAYER_ID));
    if (me < 0 || me > 32) return;
    if (g_god)  Call(N_INVINCIBLE, me, true);
    if (g_jump) Call(N_SUPERJUMP, me);
    if (!g_aim) return;

    const int selfPed = static_cast<int>(Call(N_PLAYER_PED_ID));
    if (!selfPed) return;
    const Vec3 self = Coords(selfPed);
    if (self.x == 0.0f && self.y == 0.0f) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float cx = disp.x * 0.5f, cy = disp.y * 0.5f;

    int best = 0;
    float bestD = g_fov;
    Vec3 bestPos{};

    for (int i = 0; i < 32; ++i) {
        if (i == me || !Call(N_ACTIVE, i)) continue;
        const int ped = static_cast<int>(Call(N_GET_PLAYER_PED, i));
        if (!ped) continue;
        const Vec3 pos = Coords(ped);
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

    Call(N_CAM_HEADING, bearing - heading);
    Call(N_CAM_PITCH, pitch, 1.0f);
}

const char* NameOf(int i, int ped, ULONGLONG now) {
    NameCache& c = g_names[i & 31];
    const bool stale = c.ped != ped || (now - c.at) > 2000;
    if (!stale && c.name[0]) return c.name;

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

void render() {
    if (!ready() || (!g_esp && !g_box)) return;
    if (!ImGui::GetCurrentContext()) return;
    // El ESP a medio ritmo: la mitad de llamadas por frame.
    if (g_ticks & 1) return;

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
        Vec3 head = feet;
        head.z += 1.85f;

        float fx, fy, hx, hy;
        if (!W2S(feet, fx, fy)) continue;
        if (!W2S(head, hx, hy)) continue;

        const float ddx = feet.x - self.x, ddy = feet.y - self.y, ddz = feet.z - self.z;
        const float dist = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
        if (dist > 400.0f) continue;

        float boxH = fabsf(fy - hy);
        if (boxH < 8.0f) boxH = 8.0f;
        if (boxH > 900.0f) boxH = 900.0f;
        const float boxW = boxH * 0.42f;

        if (g_box) {
            dl->AddRect(ImVec2(hx - boxW * 0.5f, hy), ImVec2(hx + boxW * 0.5f, fy),
                        IM_COL32(0, 255, 128, 200), 0.0f, 0, 1.5f);
        }

        if (g_esp) {
            const char* name = NameOf(i, ped, now);
            const int hp = static_cast<int>(Call(N_HEALTH, ped));
            char label[160]{};
            snprintf(label, sizeof(label), "%s\n%d hp\n%.0f m",
                     (name && name[0]) ? name : "jugador", hp, dist);
            dl->AddText(ImVec2(hx + boxW * 0.55f, hy), IM_COL32(0, 255, 128, 255), label);
        }
    }
}

void renderMenu() {
    if (!ready()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "natives no resueltos, revisa el log");
        ImGui::Separator();
    }

    bool e = g_esp, b = g_box, a = g_aim, g = g_god, j = g_jump;
    float f = g_fov;

    if (ImGui::Checkbox("ESP", &e))    g_esp = e;
    if (ImGui::Checkbox("Cajas", &b))  g_box = b;
    if (ImGui::Checkbox("Aimbot", &a)) g_aim = a;
    if (ImGui::SliderFloat("FOV aim", &f, 10.0f, 400.0f, "%.0f px")) g_fov = f;

    ImGui::Separator();

    if (ImGui::Checkbox("Godmode", &g))   g_god = g;
    if (ImGui::Checkbox("Superjump", &j)) g_jump = j;

    ImGui::Separator();
    ImGui::Text("proyeccion: %s", g_hasScreen ? "native" : "matriz");
}
} // namespace native