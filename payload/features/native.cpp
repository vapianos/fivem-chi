#include "features/native.h"
#include "core/lua_runtime.h"
#include "utils/log.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "imgui.h"

namespace {

struct Vec3 { float x, y, z; };

std::atomic<bool>  g_esp{ true }, g_box{ true }, g_aim{ false }, g_god{ false }, g_jump{ false };
std::atomic<bool>  g_skel{ true }, g_aimHold{ false };
std::atomic<float> g_fov{ 90.0f };
std::atomic<float> g_smooth{ 0.55f };

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_tried{ false };
const char* g_status = "esperando runtime Lua";

// ---------------------------------------------------------------- snapshot

constexpr size_t kBufSize = 24576;
char g_buf[2][kBufSize];
std::atomic<int>  g_slot{ 0 };
std::atomic<bool> g_fresh{ false };

constexpr int kMaxBones = 24;

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

// ---------------------------------------------------------------- api Lua

int api_opt(lua_State* L) {
    size_t len = 0;
    const char* k = lua::GetStringArg(L, 1, len);
    if (k && len) {
        char tmp[24]{};
        memcpy(tmp, k, len < 23 ? len : 23);
        if (!strcmp(tmp, "esp"))     { lua::PushInteger(L, g_esp ? 1 : 0); return 1; }
        if (!strcmp(tmp, "box"))     { lua::PushInteger(L, g_box ? 1 : 0); return 1; }
        if (!strcmp(tmp, "skel"))    { lua::PushInteger(L, g_skel ? 1 : 0); return 1; }
        if (!strcmp(tmp, "aim"))     { lua::PushInteger(L, g_aim ? 1 : 0); return 1; }
        if (!strcmp(tmp, "god"))     { lua::PushInteger(L, g_god ? 1 : 0); return 1; }
        if (!strcmp(tmp, "jump"))    { lua::PushInteger(L, g_jump ? 1 : 0); return 1; }
        if (!strcmp(tmp, "aimhold")) { lua::PushInteger(L, g_aimHold ? 1 : 0); return 1; }
        if (!strcmp(tmp, "fov"))     { lua::PushInteger(L, static_cast<int>(g_fov)); return 1; }
        if (!strcmp(tmp, "smooth"))  { lua::PushInteger(L, static_cast<int>(g_smooth * 100.0f)); return 1; }
    }
    lua::PushInteger(L, 0);
    return 1;
}

int api_publish(lua_State* L) {
    size_t len = 0;
    const char* s = lua::GetStringArg(L, 1, len);
    if (!s) return 0;

    const int next = 1 - g_slot.load(std::memory_order_relaxed);
    const size_t n = len < kBufSize - 1 ? len : kBufSize - 1;
    memcpy(g_buf[next], s, n);
    g_buf[next][n] = '\0';
    g_slot.store(next, std::memory_order_release);
    g_fresh.store(true, std::memory_order_release);
    return 0;
}

// ---------------------------------------------------------------- script

const char* kScript = R"LUA(
local fmt = string.format
local function opt(k) return cheat.opt(k) end

local function w2s(x, y, z, sw, sh)
    local ok, sx, sy = GetScreenCoordFromWorldCoord(x, y, z)
    if not ok or not sx or not sy then return nil end
    return sx * sw, sy * sh
end

local relH, relP, lastT = 0.0, 0.0, -1

Citizen.CreateThread(function()
    while true do
        Citizen.Wait(0)

        local me   = PlayerId()
        local ped  = PlayerPedId()
        local god  = opt("god") == 1
        local jump = opt("jump") == 1

        if god then
            SetPlayerInvincible(me, true)
            if ped and ped ~= 0 then SetEntityInvincible(ped, true) end
        end
        if jump then SetSuperJumpThisFrame(me) end

        local sw, sh = GetActiveScreenResolution()
        if not sw or sw == 0 then sw, sh = 1920, 1080 end

        local aim = opt("aim") == 1
        local hold = opt("aimhold") == 1
        if aim and hold and not IsControlPressed(0, 25) then aim = false end

        local selfC = GetEntityCoords(ped, true)
        local fov = opt("fov") + 0.0

        if aim and ped and ped ~= 0 then
            local best, bestD, bestPos = -1, fov, nil
            local cx, cy = sw * 0.5, sh * 0.5

            for i = 0, 31 do
                if i ~= me and IsPlayerActive(i) then
                    local oped = GetPlayerPed(i)
                    if oped and oped ~= 0 then
                        local oc = GetEntityCoords(oped, true)
                        local dx, dy, dz = oc.x - selfC.x, oc.y - selfC.y, oc.z - selfC.z
                        if dx*dx + dy*dy + dz*dz <= 90000.0 then
                            local sx, sy = w2s(oc.x, oc.y, oc.z, sw, sh)
                            if sx then
                                local d = math.sqrt((sx-cx)^2 + (sy-cy)^2)
                                if d < bestD then bestD, best, bestPos = d, oped, oc end
                            end
                        end
                    end
                end
            end

            if best ~= -1 and bestPos then
                local dx, dy, dz = bestPos.x - selfC.x, bestPos.y - selfC.y, bestPos.z - selfC.z
                local horiz = math.sqrt(dx*dx + dy*dy)
                if horiz > 0.1 then
                    local bearing = math.deg(math.atan(dx, dy))
                    local heading = GetEntityHeading(ped)
                    local pitch = -math.deg(math.atan(dz, horiz))
                    local rel = bearing - heading
                    while rel > 180.0 do rel = rel - 360.0 end
                    while rel < -180.0 do rel = rel + 360.0 end

                    if best ~= lastT or math.abs(rel - relH) > 60.0 then
                        relH, relP, lastT = rel, pitch, best
                    else
                        local k = opt("smooth") / 100.0
                        relH = relH + (rel - relH) * k
                        relP = relP + (pitch - relP) * k
                    end
                    SetGameplayCamRelativeHeading(relH)
                    SetGameplayCamRelativePitch(relP, 1.0)
                end
            end
        end

        local wantEsp  = opt("esp") == 1
        local wantBox  = opt("box") == 1
        local wantSkel = opt("skel") == 1
        if not wantEsp and not wantBox and not wantSkel then
            cheat.publish(fmt("H %d\n", me))
        else
            local out = { fmt("H %d\n", me) }

            for i = 0, 31 do
                if i ~= me and IsPlayerActive(i) then
                    local oped = GetPlayerPed(i)
                    if oped and oped ~= 0 then
                        local oc = GetEntityCoords(oped, true)
                        local dx, dy, dz = oc.x - selfC.x, oc.y - selfC.y, oc.z - selfC.z
                        local dist = math.sqrt(dx*dx + dy*dy + dz*dz)
                        if dist <= 500.0 then
                            local fx, fy = w2s(oc.x, oc.y, oc.z, sw, sh)
                            if fx then
                                local hc = GetPedBoneCoords(oped, 0, 0.0, 0.0, 0.0)
                                local hx, hy = w2s(hc.x, hc.y, hc.z, sw, sh)
                                if not hx then hx, hy = fx, fy - 40.0 end

                                if wantSkel and dist <= 300.0 then
                                    local bones = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                                    22, 23, 24, 25, 26, 27 }
                                    for _, b in ipairs(bones) do
                                        local bc = GetPedBoneCoords(oped, b, 0.0, 0.0, 0.0)
                                        local bx, by = w2s(bc.x, bc.y, bc.z, sw, sh)
                                        if bx then out[#out+1] = fmt("B %d %.1f %.1f\n", b, bx, by) end
                                    end
                                end

                                local name = GetPlayerName(i) or "jugador"
                                name = string.gsub(name, "%s+", "_")
                                out[#out+1] = fmt("P %d %.1f %d %.1f %.1f %.1f %.1f %s\n",
                                    i, dist, GetEntityHealth(oped), hx, hy, fx, fy, name)
                            end
                        end
                    end
                end
            end

            cheat.publish(table.concat(out))
        end
    end
end)
)LUA";

void Install() {
    if (g_installed) return;
    if (!lua::ready()) return;

    if (lua::RunString(kScript, "@cheat_snap")) {
        g_installed = true;
        g_status = "activo";
        Log("[native] script de snapshot instalado");
    } else {
        g_status = "error al instalar el script";
        LogE("[native] no se pudo instalar el script");
    }
}

// ---------------------------------------------------------------- parser

bool ParseFrame() {
    if (!g_fresh.exchange(false)) return false;

    const int slot = g_slot.load(std::memory_order_acquire);
    const char* p = g_buf[slot];
    if (!p[0]) return false;

    memset(&g_frame, 0, sizeof(g_frame));
    g_frame.me = -1;

    while (*p) {
        while (*p == '\n' || *p == '\r') ++p;
        if (!*p) break;

        const char tag = *p;
        if (tag == 'H') {
            g_frame.me = atoi(p + 1);
        } else if (tag == 'B' && g_frame.count > 0) {
            const int idx = g_frame.count - 1;
            if (g_frame.boneCount[idx] < kMaxBones) {
                const char* q = p + 1;
                BonePt& b = g_frame.bones[idx][g_frame.boneCount[idx]];
                b.id = atoi(q);
                while (*q == ' ') ++q;
                while (*q && *q != ' ') ++q;
                b.x = (float)atof(q);
                while (*q == ' ') ++q;
                while (*q && *q != ' ') ++q;
                b.y = (float)atof(q);
                ++g_frame.boneCount[idx];
            }
        } else if (tag == 'P' && g_frame.count < 32) {
            const int idx = g_frame.count;
            const char* q = p + 1;
            g_frame.slot[idx] = atoi(q);
            while (*q == ' ') ++q; while (*q && *q != ' ') ++q;
            g_frame.dist[idx] = (float)atof(q);
            while (*q == ' ') ++q; while (*q && *q != ' ') ++q;
            g_frame.hp[idx] = atoi(q);
            for (int k = 0; k < 4; ++k) {
                while (*q == ' ') ++q; while (*q && *q != ' ') ++q;
                g_frame.box[idx][k] = (float)atof(q);
            }
            while (*q == ' ') ++q;
            size_t n = 0;
            while (*q && *q != ' ' && *q != '\n' && *q != '\r' && n < 27) {
                char c = *q++;
                g_frame.name[idx][n++] = (c == '_') ? ' ' : c;
            }
            g_frame.name[idx][n] = '\0';
            ++g_frame.count;
        }

        while (*p && *p != '\n') ++p;
    }
    return true;
}

void Draw() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    for (int i = 0; i < g_frame.count; ++i) {
        const float x0 = g_frame.box[i][0], y0 = g_frame.box[i][1];
        const float x1 = g_frame.box[i][2], y1 = g_frame.box[i][3];

        float boxH = fabsf(y1 - y0);
        if (boxH < 8.0f) boxH = 8.0f;
        const float boxW = boxH * 0.42f;
        const float cx = (x0 + x1) * 0.5f;

        if (g_box) {
            dl->AddRect(ImVec2(cx - boxW * 0.5f, y0), ImVec2(cx + boxW * 0.5f, y1),
                        IM_COL32(0, 255, 128, 200), 0.0f, 0, 1.5f);
        }

        if (g_skel && g_frame.boneCount[i] > 1) {
            for (int k = 1; k < g_frame.boneCount[i]; ++k) {
                const BonePt& a = g_frame.bones[i][k - 1];
                const BonePt& b = g_frame.bones[i][k];
                dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), IM_COL32(255, 255, 255, 190), 1.2f);
            }
        }

        if (g_esp) {
            char label[96]{};
            snprintf(label, sizeof(label), "%s\n%d hp\n%.0f m",
                     g_frame.name[i][0] ? g_frame.name[i] : "jugador", g_frame.hp[i], g_frame.dist[i]);
            dl->AddText(ImVec2(cx + boxW * 0.55f, y0), IM_COL32(0, 255, 128, 255), label);
        }
    }
}
} // namespace

namespace native {

bool ready() { return g_installed.load() && lua::ready(); }
bool usingMatrix() { return true; }
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
    g_installed = false;
    g_status = "rearme manual";
    Log("rearme del script solicitado");
}

bool init() {
    if (g_tried.exchange(true)) return g_installed.load();
    if (!lua::init()) {
        g_status = "runtime Lua no disponible";
        g_tried = false;
        return false;
    }
    return g_installed.load();
}

void shutdown() {
    g_installed = false;
    g_fresh = false;
}

void addLuaFields(lua_State* L) {
    lua::AddField(L, "opt", api_opt);
    lua::AddField(L, "publish", api_publish);
}

void onLuaState(lua_State*) {
    Install();
}

void tick() {
    if (!g_installed) {
        if (lua::ready()) Install();
        return;
    }
    if (!lua::ready()) {
        g_installed = false;
        g_status = "runtime Lua perdido";
    }
}

void render() {
    if (!ready()) return;
    if (!ImGui::GetCurrentContext()) return;
    if (!g_esp && !g_box && !g_skel) return;
    if (!ParseFrame()) return;
    Draw();
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
    ImGui::Text("modo: lua");
    ImGui::Text("entidades: %d", g_frame.count);
    ImGui::Text("alcance: 500 m");

    if (!g_installed && ImGui::Button("Reintentar")) rearm();
}
} // namespace native