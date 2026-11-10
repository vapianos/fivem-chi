#include "render/menu.h"
#include "render/overlay.h"
#include "features/native.h"
#include "core/config.h"
#include "utils/paths.h"
#include "features/script_loader.h"

#include <Windows.h>
#include "imgui.h"

namespace menu {

static void BootLogTail() {
    wchar_t dir[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
        ImGui::TextDisabled("sin boot.log");
        return;
    }
    lstrcatW(dir, L"\\FiveMCheat\\boot.log");

    HANDLE f = CreateFileW(dir, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        ImGui::TextDisabled("sin boot.log");
        return;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    constexpr long long kTake = 4096;
    if (size.QuadPart > kTake) {
        LARGE_INTEGER at{};
        at.QuadPart = size.QuadPart - kTake;
        SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    }

    char buf[kTake + 1]{};
    DWORD got = 0;
    ReadFile(f, buf, (DWORD)kTake, &got, nullptr);
    CloseHandle(f);
    ImGui::TextUnformatted(buf);
}

void Render() {
    ImGui::SetNextWindowSize(ImVec2(460.0f, 420.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Venice", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Visuales")) {
            native::renderMenu();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Lua")) {
            ImGui::Text("carpeta:");
            ImGui::TextWrapped("%s", paths::ToUtf8(paths::LuaDir()).c_str());
            ImGui::Separator();
            if (ImGui::Button("Recargar scripts")) scripts::load_all();
            ImGui::TextDisabled("Los toggles de arriba son nativos y no dependen de Lua.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Ajustes")) {
            Config& c = config::Get();
            int key = c.menuKey;
            if (ImGui::InputInt("Tecla del menu (VK)", &key)) {
                if (key < 0x08 || key > 0xFE) key = VK_INSERT;
                c.menuKey = key;
            }
            ImGui::TextDisabled("Insert siempre funciona, aunque cambies esto.");

            bool autoLoad = c.loadOnStart;
            if (ImGui::Checkbox("Cargar scripts al iniciar", &autoLoad)) c.loadOnStart = autoLoad;

            if (ImGui::Button("Guardar")) config::Save();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Estado")) {
            ImGui::Text("natives: %s", native::ready() ? "ok" : "no");
            ImGui::Text("proyeccion: %s", native::usingMatrix() ? "matriz" : "native");
            ImGui::Text("overlay: %s", overlay::armed() ? "armado" : "sin armar");
            ImGui::Separator();
            ImGui::TextDisabled("F1 ESP  F2 cajas  F3 aim  F4 god  F5 jump  F6 esqueleto  F8 ocultar");

            if (!native::ready() && ImGui::Button("Reintentar resolucion de natives")) native::rearm();
            ImGui::SameLine();
            if (ImGui::Button("Rearmar overlay")) overlay::init();

            ImGui::Separator();
            ImGui::TextDisabled("ultimas lineas de boot.log");
            ImGui::BeginChild("boot", ImVec2(0, 170), true);
            BootLogTail();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
} // namespace menu