#include "features/native.h"
#include "features/natives.h"
#include "core/config.h"
#include "utils/log.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace {

struct Vec3 { float x, y, z; };

std::atomic<bool>  g_esp{ true }, g_box{ true }, g_aim{ false }, g_god{ false }, g_jump{ false };
std::atomic<bool>  g_skel{ true }, g_aimHold{ false };
std::atomic<float> g_fov{ 90.0f };
std::atomic<float> g_smooth{ 0.55f };

std::atomic<bool> g_up{ false };
const char* g_status = "sin iniciar";

constexpr int kMaxBones = 20;

struct BonePt { int id; float x, y; };

struct Frame {
    int   me = -1;
    int   count = 0;
    int   slot[32]{};
    float dist[32]{};
    int   hp[32]{};
    char  name[32][28]{};
    float box[32][4]{};
    int   boneCount[32]{};
    BonePt bones[32][kMaxBones]{};
};

Frame g_frame;
ULONGLONG g_lastTick = 0;

// --------------------------------------------------------------- helpers

int64_t Arg0(uint64_t a) { return static_cast<int64_t>(a); }

int64_t NPlayerId()     { return natives::v_int(0x4F8644AF03D0E0D6, 0, nullptr); }
int64_t NPed()          { return natives::v_int(0xD80958FC74E988A6, 0, nullptr); }

bool GetCoords(int64_t ent, Vec3& out) {
    alignas(16) uint8_t ret[64]{};
    uint64_t args[2] = { static_cast<uint64_t>(ent), 1 };
    if (!natives::call(0x3FEF770D40960D5A, ret, 2, args)) return false;
    memcpy(&out, ret, sizeof(Vec3));
    return out.x != 0.0f || out.y != 0.0f || out.z != 0.0f;
}

bool W2S(const Vec3& p, float sw, float sh, float& sx, float& sy) {
    // GET_SCREEN_COORD_FROM_WORLD_COORD(x, y, z, &sx, &sy) -> BOOL
    alignas(16) uint8_t ret[64]{};
    float ox = 0.0f, oy = 0.0f;
    uint64_t args[5] = { 0, 0, 0, 0, 0 };
    memcpy(&args[0], &p.x, 4);
    memcpy(&args[1], &p.y, 4);
    memcpy(&args[2], &p.z, 4);
    args[3] = reinterpret_cast<uint64_t>(&ox);
    args[4] = reinterpret_cast<uint64_t>(&oy);

    if (!natives::call(0x34E82F05DF2974F5, ret, 5, args)) return false;
    if (!*reinterpret_cast<uint8_t*>(ret)) return false;
    // Fuera de este rango es proyeccion detras de la camara o basura.
    if (!(ox > -0.5f && ox < 1.5f) || !(oy > -0.5f && oy < 1.5f)) return false;

    sx = ox * sw;
    sy = oy * sh;
    return true;
}

void ScreenRes(float& sw, float& sh) {
    sw = 1920.0f;
    sh = 1080.0f;
    if (!natives::has(0x873C9F3104101DD3)) return;

    alignas(16) uint8_t ret[64]{};
    int x = 0, y = 0;
    uint64_t args[2] = { reinterpret_cast<uint64_t>(&x), reinterpret_cast<uint64_t>(&y) };
    if (!natives::call(0x873C9F3104101DD3, ret, 2, args)) return;
    if (x > 0 && y > 0) { sw = static_cast<float>(x); sh = static_cast<float>(y); }
}

bool BoneCoords(int64_t ped, int bone, Vec3& out) {
    alignas(16) uint8_t ret[64]{};
    uint64_t args[5] = { static_cast<uint64_t>(ped), static_cast<uint64_t>(bone), 0, 0, 0 };
    if (!natives::call(0x17C07FC640E86B4E, ret, 5, args)) return false;
    memcpy(&out, ret, sizeof(Vec3));
    return true;
}

void PlayerName(int64_t slot, char* out, size_t n) {
    if (out && n) out[0] = '\0';
    if (!natives::has(0x6D0DE6A7B5DA71F8)) return;
    alignas(16) uint8_t ret[64]{};
    uint64_t args[1] = { static_cast<uint64_t>(slot) };
    if (!natives::call(0x6D0DE6A7B5DA71F8, ret, 1, args)) return;
    const char* s = *reinterpret_cast<const char**>(ret);
    if (!s) return;
    size_t i = 0;
    for (; s[i] && i + 1 < n; ++i) out[i] = (s[i] == ' ') ? '_' : s[i];
    out[i] = '\0';
}

// --------------------------------------------------------------- features

void DoGod() {
    if (!natives::has(0x239528EACDC3E7DE)) return;
    const int64_t me = NPlayerId();
    const int64_t ped = NPed();
    uint64_t a1[2] = { static_cast<uint64_t>(me), 1 };
    natives::v_void(0x239528EACDC3E7DE, 2, a1);
    if (ped && natives::has(0x3882114BDE571AD4)) {
        uint64_t a2[2] = { static_cast<uint64_t>(ped), 1 };
        natives::v_void(0x3882114BDE571AD4, 2, a2);
    }
}

void DoJump() {
    if (!natives::has(0x57FFF03E423A4C0B)) return;
    uint64_t a[1] = { static_cast<uint64_t>(NPlayerId()) };
    natives::v_void(0x57FFF03E423A4C0B, 1, a);
}

void DoAim(const Vec3& self, float sw, float sh) {
    if (!natives::has(0x14D6F5678D8F1B37) || !natives::has(0xB4EC2312F4E5B1F1)) return;

    const float fovPx = g_fov;
    const float cx = sw * 0.5f, cy = sh * 0.5f;
    int64_t best = 0;
    float bestD = fovPx;
    Vec3 bestPos{};

    for (int i = 0; i < 32; ++i) {
        if (i == g_frame.me) continue;
        uint64_t a[1] = { static_cast<uint64_t>(i) };
        if (!natives::v_int(0xB8DFD30D6973E135, 1, a)) continue;

        alignas(16) uint8_t ret[64]{};
        uint64_t ap[1] = { static_cast<uint64_t>(i) };
        if (!natives::call(0x43A66C31C68491C0, ret, 1, ap)) continue;
        const int64_t oped = *reinterpret_cast<int64_t*>(ret);
        if (!oped) continue;

        Vec3 oc{};
        if (!GetCoords(oped, oc)) continue;
        const float dx = oc.x - self.x, dy = oc.y - self.y, dz = oc.z - self.z;
        if (dx * dx + dy * dy + dz * dz > 90000.0f) continue;

        float sx, sy;
        if (!W2S(oc, sw, sh, sx, sy)) continue;
        const float d = sqrtf((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        if (d < bestD) { bestD = d; best = oped; bestPos = oc; }
    }
    if (!best) return;

    alignas(16) uint8_t cam[64]{};
    if (!natives::call(0x14D6F5678D8F1B37, cam, 0, nullptr)) return;
    Vec3 cc{};
    memcpy(&cc, cam, sizeof(Vec3));

    const float dx = bestPos.x - cc.x, dy = bestPos.y - cc.y, dz = bestPos.z - cc.z;
    const float horiz = sqrtf(dx * dx + dy * dy);
    if (horiz < 0.1f) return;

    const float bearing = atan2f(dx, dy) * 57.29578f;
    const float pitch   = -atan2f(dz, horiz) * 57.29578f;

    uint64_t ah[1] = { 0 };
    float bh = bearing;
    memcpy(&ah[0], &bh, 4);
    natives::v_void(0xB4EC2312F4E5B1F1, 1, ah);

    if (natives::has(0x6D0858B8EDFD2B7D)) {
        uint64_t ap[2] = { 0, 0 };
        float bp = pitch, one = 1.0f;
        memcpy(&ap[0], &bp, 4);
        memcpy(&ap[1], &one, 4);
        natives::v_void(0x6D0858B8EDFD2B7D, 2, ap);
    }
}

void BuildEsp(const Vec3& self, float sw, float sh) {
    memset(&g_frame, 0, sizeof(g_frame));
    g_frame.me = static_cast<int>(NPlayerId());

    const bool wantEsp = g_esp, wantBox = g_box, wantSkel = g_skel;
    if (!wantEsp && !wantBox && !wantSkel) return;

    for (int i = 0; i < 32; ++i) {
        if (i == g_frame.me) continue;
        uint64_t a[1] = { static_cast<uint64_t>(i) };
        if (!natives::v_int(0xB8DFD30D6973E135, 1, a)) continue;

        alignas(16) uint8_t ret[64]{};
        uint64_t ap[1] = { static_cast<uint64_t>(i) };
        if (!natives::call(0x43A66C31C68491C0, ret, 1, ap)) continue;
        const int64_t oped = *reinterpret_cast<int64_t*>(ret);
        if (!oped) continue;

        Vec3 oc{};
        if (!GetCoords(oped, oc)) continue;
        const float dx = oc.x - self.x, dy = oc.y - self.y, dz = oc.z - self.z;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist > 500.0f) continue;

        float fx, fy;
        if (!W2S(oc, sw, sh, fx, fy)) continue;

        Vec3 hc{};
        float hx = fx, hy = fy - 40.0f;
        if (BoneCoords(oped, 31086, hc) && W2S(hc, sw, sh, hx, hy)) { /* ok */ }

        const int idx = g_frame.count;
        if (idx >= 32) break;
        g_frame.slot[idx] = i;
        g_frame.dist[idx] = dist;
        g_frame.box[idx][0] = fx;
        g_frame.box[idx][1] = hy;
        g_frame.box[idx][2] = fx;
        g_frame.box[idx][3] = fy;
        g_frame.hp[idx] = static_cast<int>(natives::v_int(0xEEF059FAD016D209, 1, ap));
        PlayerName(i, g_frame.name[idx], sizeof(g_frame.name[idx]));

        if (wantSkel && dist <= 300.0f) {
            static const int kBones[] = {
                31086, 39317, 24818, 11816,
                45509, 61163, 18905,
                40269, 28252, 57005,
                58271, 63931, 14201,
                51826, 36864, 52301
            };
            for (int b : kBones) {
                Vec3 bc{};
                if (!BoneCoords(oped, b, bc)) continue;
                float bx, by;
                if (!W2S(bc, sw, sh, bx, by)) continue;
                if (g_frame.boneCount[idx] >= kMaxBones) break;
                BonePt& p = g_frame.bones[idx][g_frame.boneCount[idx]++];
                p.id = b; p.x = bx; p.y = by;
            }
        }
        ++g_frame.count;
    }
}

const BonePt* FindBone(int idx, int id) {
    for (int k = 0; k < g_frame.boneCount[idx]; ++k)
        if (g_frame.bones[idx][k].id == id) return &g_frame.bones[idx][k];
    return nullptr;
}

void Draw() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    static const int kLinks[][2] = {
        { 31086, 39317 }, { 39317, 24818 }, { 24818, 11816 },
        { 39317, 45509 }, { 45509, 61163 }, { 61163, 18905 },
        { 39317, 40269 }, { 40269, 28252 }, { 28252, 57005 },
        { 11816, 58271 }, { 58271, 63931 }, { 63931, 14201 },
        { 11816, 51826 }, { 51826, 36864 }, { 36864, 52301 },
    };

    for (int i = 0; i < g_frame.count; ++i) {
        const float x0 = g_frame.box[i][0], y0 = g_frame.box[i][1];
        const float x1 = g_frame.box[i][2], y1 = g_frame.box[i][3];

        float boxH = fabsf(y1 - y0);
        if (boxH < 8.0f) boxH = 8.0f;
        const float boxW = boxH * 0.42f;
        const float cx = (x0 + x1) * 0.5f;

        if (g_box)
            dl->AddRect(ImVec2(cx - boxW * 0.5f, y0), ImVec2(cx + boxW * 0.5f, y1),
                        IM_COL32(0, 255, 128, 200), 0.0f, 0, 1.5f);

        if (g_skel && g_frame.boneCount[i] > 1) {
            for (const auto& link : kLinks) {
                const BonePt* a = FindBone(i, link[0]);
                const BonePt* b = FindBone(i, link[1]);
                if (a && b)
                    dl->AddLine(ImVec2(a->x, a->y), ImVec2(b->x, b->y), IM_COL32(255, 255, 255, 190), 1.2f);
            }
        }

        if (g_esp) {
            char label[96]{};
            const char* nm = g_frame.name[i][0] ? g_frame.name[i] : "jugador";
            snprintf(label, sizeof(label), "%s\n%d hp\n%.0f m", nm, g_frame.hp[i], g_frame.dist[i]);
            dl->AddText(ImVec2(cx + boxW * 0.55f, y0), IM_COL32(0, 255, 128, 255), label);
        }
    }
}
} // namespace

namespace native {

bool ready() { return g_up.load() && natives::ready(); }
bool usingMatrix() { return false; }
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
    g_up = false;
    g_status = "rearme manual";
    Log("rearme de nativas solicitado");
    if (natives::init()) { g_up = true; g_status = "activo"; }
}

bool init() {
    if (!natives::init()) { g_status = natives::statusText(); return false; }
    g_up = true;
    g_status = "activo";
    Log("[native] nativas listas");
    return true;
}

void shutdown() { g_up = false; }

void tick() {
    const ULONGLONG now = GetTickCount64();
    if (!ready()) {
        static ULONGLONG s_lastRetry = 0;
        if (config::Get().nativesEnabled && now - s_lastRetry >= 2000) {
            s_lastRetry = now;
            if (natives::init()) {
                g_up = true;
                g_status = "activo";
                Log("[native] nativas listas");
            } else {
                g_status = natives::statusText();
            }
        }
        return;
    }

    if (now - g_lastTick < 4) return;   // ~250 Hz max, no saturar el Present
    g_lastTick = now;

    const int64_t ped = NPed();
    if (!ped) return;

    Vec3 self{};
    if (!GetCoords(ped, self)) return;

    if (g_god)  DoGod();
    if (g_jump) DoJump();

    float sw, sh;
    ScreenRes(sw, sh);

    BuildEsp(self, sw, sh);

    bool wantAim = g_aim;
    if (wantAim && g_aimHold && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) wantAim = false;
    if (wantAim) DoAim(self, sw, sh);
}

void render() {
    if (!ready() || !ImGui::GetCurrentContext()) return;
    if (g_esp || g_box || g_skel) Draw();
}

void renderMenu() {
    if (!ready()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "estado: %s", g_status);
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
    ImGui::Text("modo: nativo");
    ImGui::Text("entidades: %d", g_frame.count);
    ImGui::Text("alcance: 500 m");

    if (!g_up && ImGui::Button("Reintentar")) rearm();
}
} // namespace native