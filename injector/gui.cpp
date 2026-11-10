#include "gui.h"
#include "inject.h"

#include <Windows.h>
#include <commdlg.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "shell32.lib")

namespace {

struct Entry {
    DWORD       pid;
    std::string name;
    int         rank;   // 0 = no sirve, 1 = FiveM genérico, 2 = proceso del juego
};

std::vector<Entry>       g_processes;
char                     g_filter[64] = "";
char                     g_dllPath[MAX_PATH] = "";
std::vector<std::string> g_log;
int                      g_selected = -1;

DWORD       g_targetPid  = 0;
std::string g_targetName;
bool        g_autoDetect = true;
bool        g_autoInject = false;
bool        g_injected   = false;
double      g_lastScan   = 0.0;

int g_method = 0;   // 0 = LoadLibrary (recomendado), 1 = manual map

void AddLog(const std::string& text) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[512]{};
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, text.c_str());
    g_log.push_back(line);
    if (g_log.size() > 200) g_log.erase(g_log.begin());
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ContainsCI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    return Lower(hay).find(Lower(needle)) != std::string::npos;
}

// 2 = proceso del juego (GTAProcess), 1 = FiveM pero no el juego, 0 = irrelevante
int RankOf(const std::string& rawName) {
    const std::string n = Lower(rawName);

    if (n.find("chromebrowser") != std::string::npos) return 0;
    if (n.find("dumpserver")    != std::string::npos) return 0;
    if (n.find("roslauncher")   != std::string::npos) return 0;
    if (n.find("rosservice")    != std::string::npos) return 0;
    if (n.find("crashhandler")  != std::string::npos) return 0;

    if (n.find("gtaprocess") != std::string::npos) return 2;
    if (n == "fivem.exe") return 1;
    if (n.find("fivem") != std::string::npos) return 1;

    return 0;
}

bool Is64Bit(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;

    BOOL wow64 = FALSE;
    bool ok = true;
    if (IsWow64Process(h, &wow64) && wow64)
        ok = false;

    CloseHandle(h);
    return ok;
}

bool StillAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;

    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

void SetTarget(const Entry& e) {
    g_targetPid  = e.pid;
    g_targetName = e.name;
    g_injected   = false;
}

void ClearTarget() {
    g_targetPid = 0;
    g_targetName.clear();
    g_selected = -1;
    g_injected = false;
}

void AutoDetect() {
    if (!StillAlive(g_targetPid))
        ClearTarget();

    int bestRank = 0;
    int bestIdx  = -1;

    for (int i = 0; i < static_cast<int>(g_processes.size()); ++i) {
        const auto& e = g_processes[i];
        if (e.rank > bestRank) {
            bestRank = e.rank;
            bestIdx  = i;
        }
    }

    if (bestIdx >= 0) {
        const auto& e = g_processes[bestIdx];
        if (e.pid != g_targetPid) {
            SetTarget(e);
            g_selected = bestIdx;
            AddLog("FiveM detectado: " + e.name + " (" + std::to_string(e.pid) + ")");
        }
    } else if (g_autoDetect && g_targetPid) {
        ClearTarget();
    }
}

// El build deja cheat.dll junto al launcher y tambien en payload\. payload.dll ya no se usa.
std::string DefaultDllPath() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring base = exe;
    const auto pos = base.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        base = base.substr(0, pos + 1);

    const std::wstring candidates[] = {
        base + L"cheat.dll",
        base + L"payload\\cheat.dll",
        base + L"payload.dll",
    };

    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            char out[MAX_PATH]{};
            WideCharToMultiByte(CP_ACP, 0, c.c_str(), -1, out, MAX_PATH, nullptr, nullptr);
            return out;
        }
    }

    char out[MAX_PATH]{};
    WideCharToMultiByte(CP_ACP, 0, (base + L"cheat.dll").c_str(), -1, out, MAX_PATH, nullptr, nullptr);
    return out;
}

bool DllExists() {
    return GetFileAttributesA(g_dllPath) != INVALID_FILE_ATTRIBUTES;
}

void RelaunchAsAdmin() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", exe, nullptr, nullptr, SW_SHOWNORMAL)) > 32)
        ExitProcess(0);
}

void DoInject() {
    if (!g_targetPid) return;

    if (!DllExists()) {
        AddLog("ERROR: no existe " + std::string(g_dllPath));
        return;
    }

    const inj::Method method = (g_method == 1) ? inj::Method::ManualMap : inj::Method::LoadLibrary;
    AddLog(std::string("inyectando [") + (g_method == 1 ? "manual map" : "loadlibrary") + "] en " +
           g_targetName + " (" + std::to_string(g_targetPid) + ")");

    wchar_t wide[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, g_dllPath, -1, wide, MAX_PATH);

    std::string error;
    if (inj::Inject(g_targetPid, wide, method, error)) {
        AddLog("OK: payload inyectado. Pulsa INSERT en el juego para abrir el menu.");
        if (g_method == 1)
            AddLog("nota: con manual map el modulo no aparece en la lista, Unload no funcionara.");
        g_injected = true;
    } else {
        AddLog("ERROR: " + error);
    }
}

void DoEject() {
    if (!g_targetPid) return;
    std::string error;
    if (inj::Eject(g_targetPid, error)) {
        AddLog("payload descargado");
        g_injected = false;
    } else {
        AddLog("ERROR: " + error);
    }
}
} // namespace

namespace gui {

void LogLine(const char* text) { AddLog(text); }

void Refresh() {
    g_processes.clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring w = pe.szExeFile;
            std::string name(w.begin(), w.end());

            const int rank = RankOf(name);
            if (rank == 0) continue;
            if (!Is64Bit(pe.th32ProcessID)) continue;

            g_processes.push_back({ pe.th32ProcessID, name, rank });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(g_processes.begin(), g_processes.end(), [](const Entry& a, const Entry& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        return a.pid < b.pid;
    });

    g_selected = -1;
    for (int i = 0; i < static_cast<int>(g_processes.size()); ++i) {
        if (g_processes[i].pid == g_targetPid) { g_selected = i; break; }
    }
}

void Render() {
    if (!g_dllPath[0]) {
        const std::string def = DefaultDllPath();
        strncpy_s(g_dllPath, def.c_str(), sizeof(g_dllPath) - 1);
    }

    if (g_autoDetect && ImGui::GetTime() - g_lastScan > 1.0) {
        g_lastScan = ImGui::GetTime();
        Refresh();
        AutoDetect();
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("fivem-cheat injector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // ---- privilegios ----
    if (!inj::IsElevated()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "Sin privilegios de administrador");
        ImGui::SameLine();
        if (ImGui::Button("Reiniciar como admin"))
            RelaunchAsAdmin();
    }

    // ---- estado ----
    if (g_targetPid && StillAlive(g_targetPid)) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "FiveM listo: %s [%u]",
                           g_targetName.c_str(), g_targetPid);
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "Esperando a FiveM...");
        ImGui::SameLine();
        ImGui::TextDisabled("(abre el juego y entra a un servidor)");
    }

    ImGui::Checkbox("Detectar automatico", &g_autoDetect);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-inyectar al detectar", &g_autoInject);

    ImGui::Separator();

    // ---- DLL ----
    ImGui::TextUnformatted("DLL:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-160);
    ImGui::InputText("##dll", g_dllPath, sizeof(g_dllPath));
    ImGui::SameLine();
    if (ImGui::Button("Examinar")) {
        OPENFILENAMEW ofn{};
        wchar_t buf[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, g_dllPath, -1, buf, MAX_PATH);
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = L"DLL (*.dll)\0*.dll\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        if (GetOpenFileNameW(&ofn))
            WideCharToMultiByte(CP_ACP, 0, buf, -1, g_dllPath, MAX_PATH, nullptr, nullptr);
    }
    ImGui::SameLine();
    if (!DllExists())
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "no encontrada");
    else
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "ok");

    // ---- metodo ----
    ImGui::TextUnformatted("Metodo:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##method", &g_method, "LoadLibrary\0Manual map\0");
    ImGui::SameLine();
    ImGui::TextDisabled(g_method == 0 ? "(recomendado)" : "(no aparece en la lista de modulos)");

    ImGui::Separator();

    // ---- filtro ----
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "filtrar...", g_filter, sizeof(g_filter));
    ImGui::SameLine();
    if (ImGui::Button("Refrescar")) { Refresh(); AutoDetect(); }

    // ---- lista ----
    ImGui::BeginChild("##list", ImVec2(0, -160), true);
    if (g_processes.empty())
        ImGui::TextDisabled("Sin procesos de FiveM");

    for (int i = 0; i < static_cast<int>(g_processes.size()); ++i) {
        const auto& e = g_processes[i];
        if (!ContainsCI(e.name, g_filter)) continue;

        const bool isTarget = e.pid == g_targetPid;
        if (isTarget)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.95f, 0.4f, 1.0f));

        char label[320]{};
        snprintf(label, sizeof(label), "%s%s  [%u]",
                 e.rank == 2 ? "* " : "  ", e.name.c_str(), e.pid);

        if (ImGui::Selectable(label, isTarget)) {
            SetTarget(e);
            g_selected = i;
            g_autoDetect = false;
        }

        if (isTarget) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // ---- botones ----
    const bool canLaunch = g_targetPid != 0 && StillAlive(g_targetPid) && DllExists() && inj::IsElevated();
    if (!canLaunch) ImGui::BeginDisabled();

    if (ImGui::Button(g_injected ? "Inyectar de nuevo" : "Launch", ImVec2(160, 34)))
        DoInject();

    if (!canLaunch) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Unload", ImVec2(90, 34)))
        DoEject();

    ImGui::SameLine();
    if (ImGui::Button("Limpiar log", ImVec2(120, 34)))
        g_log.clear();

    // ---- auto-inyeccion ----
    if (g_autoInject && canLaunch && !g_injected && g_targetPid) {
        static DWORD lastPid = 0;
        if (lastPid != g_targetPid) {
            lastPid = g_targetPid;
            DoInject();
        }
    }

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0, 0), true);
    for (auto& line : g_log)
        ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();

    ImGui::End();
}

void frame() { Render(); }
} // namespace gui