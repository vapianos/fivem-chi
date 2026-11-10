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

#include "imgui.h"

namespace {

using Handler = void(*)(void*);

struct Reg {
    Reg*     next;
    uint64_t hash;
    Handler  handler;
};

#pragma pack(push, 1)
struct Vec3 { float x, y, z; };

// scrNativeCallContext del juego. Layout exacto:
//   0x00 ret | 0x08 argCount | 0x0C dataCount | 0x10 args | 0x18 nativeInit
//   0x40 en adelante es libre y se usa como almacen.
struct alignas(16) Ctx {
    void*    ret;
    uint32_t argCount;
    uint32_t dataCount;
    void**   args;
    uint64_t nativeInit;
    uint8_t  pad[0x20];
    void*    slots[32];
    uint64_t rets[8];
    Vec3     vecs[8];
};
#pragma pack(pop)

Reg**     g_table = nullptr;
uintptr_t g_game  = 0;
size_t    g_size  = 0;
bool      g_hasScreen = false;
int       g_screenFails = 0;

std::atomic<bool>  g_esp{ true }, g_box{ true }, g_aim{ false }, g_god{ false }, g_jump{ false };
std::atomic<float> g_fov{ 90.0f };

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

void Begin(Ctx& c) { c = {}; c.args = c.slots; c.ret = c.rets; }

void Push(Ctx& c, int v)    { c.args[c.argCount++] = (void*)(intptr_t)v; }
void Push(Ctx& c, bool v)   { c.args[c.argCount++] = (void*)(intptr_t)(v ? 1 : 0); }
void Push(Ctx& c, float v)  { uint32_t b; memcpy(&b, &v, 4); c.args[c.argCount++] = (void*)(uintptr_t)b; }
void Push(Ctx& c, void* p)  { c.args[c.argCount++] = p; }
void Push(Ctx& c, float* p) { c.args[c.argCount++] = p; }
void Push(Ctx& c, Vec3* v)  { c.args[c.argCount++] = v; ++c.dataCount; }

// El handler escribe el retorno en c.rets segun el tipo declarado en el hash.
// Para natives float el valor ocupa 4 bytes y el resto queda a cero: hay que
// releerlo como float, nunca castear el uint64.
float AsFloat(uint64_t raw) {
    float f = 0.0f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

Handler Get(uint64_t hash) {
    if (!g_table) return nullptr;
    for (Reg* r = g_table[hash & 0xFF]; r; r = r->next)
        if (r->hash == hash) return r->handler;
    return nullptr;
}

uint64_t Invoke(uint64_t hash, Ctx& c) {
    Handler h = Get(hash);
    if (!h) return 0;
    h(&c);
    return c.rets[0];
}

template <typename... A>
uint64_t Call(uint64_t hash, A... a) {
    Ctx c; Begin(c);
    (Push(c, a), ...);
    return Invoke(hash, c);
}

Vec3 Coords(int ped) {
    Vec3 out{};
    if (!ped || !g_table) return out;
    Ctx c; Begin(c);
    Push(c, ped);
    Push(c, true);
    Push(c, &out);
    Invoke(N_COORDS, c);
    return out;
}

Vec3 CamCoord() {
    Vec3 out{};
    if (!g_table) return out;
    Ctx c; Begin(c);
    Push(c, &out);
    Invoke(N_CAM_COORD, c);
    return out;
}

Vec3 CamRot() {
    Vec3 out{};
    if (!g_table) return out;
    Ctx c; Begin(c);
    Push(c, 2);
    Push(c, &out);
    Invoke(N_CAM_ROT, c);
    return out;
}

float CamFov() {
    if (!g_table) return 0.0f;
    Ctx c; Begin(c);
    return AsFloat(Invoke(N_CAM_FOV, c));
}

// Proyeccion por matriz de camara. Respaldo para cuando el native de pantalla
// no existe o no devuelve nada en builds nuevas.
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
    if (!g_table) return false;

    if (g_hasScreen) {
        float x = 0.0f, y = 0.0f;
        Ctx c; Begin(c);
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

bool ValidTable(Reg** t) {
    __try {
        if (reinterpret_cast<uintptr_t>(t) < 0x10000) return false;
        const uint64_t probe[3] = { N_PLAYER_PED_ID, N_PLAYER_ID, N_COORDS };
        for (int p = 0; p < 3; ++p) {
            Reg* r = t[probe[p] & 0xFF];
            for (int guard = 0; r && guard < 64; ++guard, r = r->next)
                if (r->hash == probe[p])
                    return scanner::InExecutableSection(reinterpret_cast<uintptr_t>(r->handler));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
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

    const char* patterns[] = {
        "48 8D 0D ? ? ? ? 48 8B D7 E8",
        "48 8D 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 8B CF",
        "48 8B 0D ? ? ? ? 48 8B D7 E8",
        "48 8B 0D ? ? ? ? 48 8B D7 E8 ? ? ? ? 48 8B CF",
    };

    for (const char* pat : patterns) {
        auto hits = scanner::FindAll(mod, pat, 8);
        Log("[native] patron '%s' -> %zu coincidencias", pat, hits.size());
        for (uintptr_t at : hits) {
            const int32_t rel = *reinterpret_cast<int32_t*>(at + 3);
            auto* candidate = reinterpret_cast<Reg**>(at + 7 + rel);
            if (!ValidTable(candidate)) continue;
            g_table = candidate;
            Log("[native] tabla validada en %p", static_cast<void*>(g_table));
            return true;
        }
    }

    LogE("[native] tabla de natives no resuelta");
    return false;
}
} // namespace

namespace native {

bool ready() { return g_table != nullptr; }
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
    if (g_table) return true;
    if (!Resolve()) return false;

    g_hasScreen = Get(N_SCREEN) != nullptr;
    Log("[native] native de pantalla %s", g_hasScreen ? "disponible" : "ausente, se usa matriz");

    // La tabla ya esta validada: aunque el autotest falle se mantiene, para que
    // ESP/aimbot puedan reintentar en cuanto haya jugador.
    for (int i = 0; i < 60; ++i) {
        const int ped = static_cast<int>(Call(N_PLAYER_PED_ID));
        if (ped) {
            const Vec3 p = Coords(ped);
            if (fabsf(p.x) > 1.0f || fabsf(p.y) > 1.0f) {
                float sx = 0.0f, sy = 0.0f;
                Log("[native] autotest ok ped=%d coords=%.1f %.1f %.1f w2s=%d matriz=%d",
                    ped, p.x, p.y, p.z, W2S(p, sx, sy) ? 1 : 0, usingMatrix() ? 1 : 0);
                return true;
            }
            LogE("[native] autotest: coords nulas para ped %d", ped);
        }
        Sleep(500);
    }

    LogE("[native] autotest fallido, la tabla se conserva");
    return true;
}

void shutdown() { g_table = nullptr; }

void tick() {
    if (!g_table) return;

    const int me = static_cast<int>(Call(N_PLAYER_ID));
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

void render() {
    if (!g_table || (!g_esp && !g_box)) return;
    if (!ImGui::GetCurrentContext()) return;

    const int me = static_cast<int>(Call(N_PLAYER_ID));
    const int selfPed = static_cast<int>(Call(N_PLAYER_PED_ID));
    if (!selfPed) return;
    const Vec3 self = Coords(selfPed);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

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
            const char* rawName = reinterpret_cast<const char*>(Call(N_NAME, i));
            const int hp = static_cast<int>(Call(N_HEALTH, ped));
            char label[160]{};
            snprintf(label, sizeof(label), "%s\n%d hp\n%.0f m",
                     (rawName && *rawName) ? rawName : "jugador", hp, dist);
            dl->AddText(ImVec2(hx + boxW * 0.55f, hy), IM_COL32(0, 255, 128, 255), label);
        }
    }
}

void renderMenu() {
    if (!g_table) {
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