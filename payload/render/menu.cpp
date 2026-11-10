#include "render/menu.h"
#include "features/native.h"
#include "core/config.h"
#include "utils/paths.h"
#include "features/script_loader.h"

#include <Windows.h>
#include "imgui.h"

namespace menu {

void Render() {
    ImGui::SetNextWindowSize(ImVec2(430.0f, 360.0f), ImGuiCond_FirstUseEver);

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
            if (ImGui::InputInt("Tecla del menu (VK)", &key)) c.menuKey = key;
            ImGui::TextDisabled("Insert siempre funciona, aunque cambies esto.");

            bool autoLoad = c.loadOnStart;
            if (ImGui::Checkbox("Cargar scripts al iniciar", &autoLoad)) c.loadOnStart = autoLoad;

            if (ImGui::Button("Guardar")) config::Save();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Estado")) {
            ImGui::Text("natives: %s", native::ready() ? "ok" : "no");
            ImGui::Text("proyeccion: %s", native::usingMatrix() ? "matriz" : "native");
            ImGui::Separator();
            ImGui::TextDisabled("F1 ESP  F2 cajas  F3 aim  F4 god  F5 jump  F8 ocultar");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
} // namespace menu