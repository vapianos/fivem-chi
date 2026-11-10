#include <windows.h>
#include <d3d11.h>
#include <cwchar>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include "gui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static IDXGISwapChain* g_sc;
static bool g_run = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_DESTROY) { g_run = false; PostQuitMessage(0); }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"launcher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(L"launcher", L"fivem-cheat injector", WS_OVERLAPPEDWINDOW,
        200, 200, 660, 560, nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        wchar_t msg[128]{};
        swprintf_s(msg, L"D3D11CreateDeviceAndSwapChain fallo: 0x%08X", hr);
        MessageBoxW(nullptr, msg, L"launcher", MB_ICONERROR);
        return 1;
    }

    ID3D11Texture2D* back;
    g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    MSG msg{};
    while (g_run) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!IsWindowVisible(hwnd)) continue;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        gui::frame();
        ImGui::Render();

        const float c[4]{ 0.06f,0.06f,0.08f,1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, c);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}