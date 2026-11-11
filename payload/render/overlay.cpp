#include "render/overlay.h"
#include "render/menu.h"
#include "features/native.h"
#include "core/config.h"
#include "utils/log.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <atomic>
#include <exception>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Present_t    = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1_t   = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuf_t  = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// IDXGIObject 0-6, IDXGIDeviceSubObject 7, Present 8, ResizeBuffers 13,
// IDXGISwapChain1: GetDesc1 18, GetFullscreenDesc 19, GetHwnd 20,
// GetCoreWindow 21, Present1 22.
constexpr size_t kIdxPresent  = 8;
constexpr size_t kIdxResize   = 13;
constexpr size_t kIdxPresent1 = 22;

constexpr int kFaultLimit = 30;

// Los originales se capturan UNA vez, al armar, y no se re-derivan nunca.
// FiveM tambien engancha Present: si su trampolin se recrea y nosotros
// releyeramos la casilla, o_present acabaria apuntando a memoria muerta y el
// hilo principal se va de cabeza.
Present_t   o_present  = nullptr;
Present1_t  o_present1 = nullptr;
ResizeBuf_t o_resize   = nullptr;
WNDPROC     o_wndproc  = nullptr;
void**      g_vtable   = nullptr;

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
HWND                    g_hwnd    = nullptr;
IDXGISwapChain*         g_swap    = nullptr;

std::atomic<bool> g_visible{ false };
std::atomic<bool> g_toggleReq{ false };
volatile bool     g_ready        = false;
volatile bool     g_armed        = false;
volatile bool     g_inPresent    = false;
volatile bool     g_shuttingDown = false;
volatile bool     g_dead         = false;
bool              g_loggedFirst  = false;
ULONGLONG         g_frames       = 0;
ULONGLONG         g_autoShowAt   = 0;
ULONGLONG         g_nextSetupAt  = 0;
int               g_faults       = 0;

RECT  g_savedClip{};
bool  g_hadClip = false;
POINT g_lastMouse{};

int PrimaryKey() {
    const int k = config::Get().menuKey;
    if (k < 0x08 || k > 0xFE) return VK_INSERT;
    return k;
}

constexpr LONG kQueueSize = 256;
struct Msg { UINT msg; WPARAM wp; LPARAM lp; };
Msg           g_queue[kQueueSize];
volatile LONG g_head = 0;
volatile LONG g_tail = 0;

void Enqueue(UINT msg, WPARAM wp, LPARAM lp) {
    const LONG h = g_head;
    if (h - g_tail >= kQueueSize) return;
    g_queue[h % kQueueSize] = { msg, wp, lp };
    MemoryBarrier();
    g_head = h + 1;
}

bool Dequeue(Msg& out) {
    if (g_tail >= g_head) return false;
    out = g_queue[g_tail % kQueueSize];
    MemoryBarrier();
    ++g_tail;
    return true;
}

struct D3DState {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    UINT viewports = 1;
    D3D11_VIEWPORT vp{};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    FLOAT blend[4]{};
    UINT mask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT stencil = 0;
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ID3D11Buffer* ib = nullptr; UINT ibStride = 0, ibOff = 0; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOff = 0;
};

void SaveState(D3DState& s) {
    if (!g_context) return;
    g_context->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    s.viewports = 1;
    g_context->RSGetViewports(&s.viewports, &s.vp);
    g_context->RSGetState(&s.rs);
    g_context->OMGetBlendState(&s.bs, s.blend, &s.mask);
    g_context->OMGetDepthStencilState(&s.ds, &s.stencil);
    g_context->PSGetShader(&s.ps, nullptr, nullptr);
    g_context->VSGetShader(&s.vs, nullptr, nullptr);
    g_context->IAGetInputLayout(&s.il);
    g_context->IAGetPrimitiveTopology(&s.topo);
    g_context->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOff);
    g_context->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff);
}

void RestoreState(D3DState& s) {
    if (g_context) {
        g_context->OMSetRenderTargets(1, &s.rtv, s.dsv);
        if (s.viewports) g_context->RSSetViewports(s.viewports, &s.vp);
        g_context->RSSetState(s.rs);
        g_context->OMSetBlendState(s.bs, s.blend, s.mask);
        g_context->OMSetDepthStencilState(s.ds, s.stencil);
        g_context->PSSetShader(s.ps, nullptr, 0);
        g_context->VSSetShader(s.vs, nullptr, 0);
        g_context->IASetInputLayout(s.il);
        g_context->IASetPrimitiveTopology(s.topo);
        g_context->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOff);
        g_context->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff);
    }

    if (s.rtv) s.rtv->Release();
    if (s.dsv) s.dsv->Release();
    if (s.rs)  s.rs->Release();
    if (s.bs)  s.bs->Release();
    if (s.ds)  s.ds->Release();
    if (s.ps)  s.ps->Release();
    if (s.vs)  s.vs->Release();
    if (s.il)  s.il->Release();
    if (s.ib)  s.ib->Release();
    if (s.vb)  s.vb->Release();
}

bool PatchSlot(void** vtable, size_t index, void* hook, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
        LogE("VirtualProtect vtable[%zu] fallo: %lu", index, GetLastError());
        return false;
    }
    if (original) *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return true;
}

// Reafirma NUESTRA casilla sin tocar el original.
void ReassertSlot(void** vtable, size_t index, void* hook) {
    if (!vtable || !hook || vtable[index] == hook) return;
    DWORD old = 0;
    if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[index] = hook;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    }
}

void RestoreSlot(void** vtable, size_t index, void* original) {
    if (!vtable || !original) return;
    DWORD old = 0;
    if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[index] = original;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    }
}

void ReleaseRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateRtv(IDXGISwapChain* swap) {
    ReleaseRtv();
    if (!g_device || !swap) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back)
        return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    if (FAILED(hr) || !g_rtv) { g_rtv = nullptr; return false; }
    return true;
}

void ApplyVisible(bool state) {
    if (state == g_visible) return;
    g_visible = state;

    if (state) {
        g_hadClip = GetClipCursor(&g_savedClip) != 0;
        GetCursorPos(&g_lastMouse);
        ClipCursor(nullptr);
    } else {
        if (g_hadClip) ClipCursor(&g_savedClip);
        g_hadClip = false;
    }

    if (ImGui::GetCurrentContext())
        ImGui::GetIO().MouseDrawCursor = state;
}

void HandleKey(int vk) {
    if (vk == VK_INSERT || vk == PrimaryKey()) { ApplyVisible(!g_visible); return; }
    switch (vk) {
    case VK_F8: ApplyVisible(false); break;
    case VK_F1: native::setEsp(!native::esp());   Log("ESP %s", native::esp() ? "on" : "off"); break;
    case VK_F2: native::setBox(!native::box());   Log("cajas %s", native::box() ? "on" : "off"); break;
    case VK_F3: native::setAim(!native::aim());   Log("aimbot %s", native::aim() ? "on" : "off"); break;
    case VK_F4: native::setGod(!native::god());   Log("godmode %s", native::god() ? "on" : "off"); break;
    case VK_F5: native::setJump(!native::jump()); Log("superjump %s", native::jump() ? "on" : "off"); break;
    case VK_F6: native::setSkel(!native::skel()); Log("esqueleto %s", native::skel() ? "on" : "off"); break;
    default: break;
    }
}

void PollKeys() {
    if (g_toggleReq.exchange(false)) ApplyVisible(!g_visible);

    static const int keys[] = { VK_INSERT, VK_F8, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6 };
    static bool down[sizeof(keys) / sizeof(keys[0])]{};
    static bool extraDown = false;

    const bool focused = g_hwnd && GetForegroundWindow() == g_hwnd;

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const bool d = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (d && !down[i] && (focused || keys[i] == VK_INSERT)) HandleKey(keys[i]);
        down[i] = d;
    }

    const int extra = PrimaryKey();
    if (extra && extra != VK_INSERT) {
        const bool d = (GetAsyncKeyState(extra) & 0x8000) != 0;
        if (d && !extraDown && focused) HandleKey(extra);
        extraDown = d;
    }
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!o_wndproc) return DefWindowProcW(hwnd, msg, wp, lp);
    if (!g_visible) return CallWindowProcW(o_wndproc, hwnd, msg, wp, lp);

    __try {
        Enqueue(msg, wp, lp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogE("excepcion encolando mensaje 0x%04X", msg);
    }

    if (msg == WM_MOUSEMOVE) {
        POINT p{ static_cast<LONG>(static_cast<short>(LOWORD(lp))),
                 static_cast<LONG>(static_cast<short>(HIWORD(lp))) };
        if (ClientToScreen(hwnd, &p)) g_lastMouse = p;
    }

    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return 0;
    default:
        return CallWindowProcW(o_wndproc, hwnd, msg, wp, lp);
    }
}

bool Setup(IDXGISwapChain* swap) {
    if (g_ready) return true;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || !desc.OutputWindow) return false;

    DWORD owner = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &owner);
    if (owner != GetCurrentProcessId()) return false;

    ID3D11Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev)
        return false;

    g_device = dev;
    g_device->GetImmediateContext(&g_context);
    if (!g_context) {
        LogE("sin contexto inmediato, overlay no dibuja");
        g_device->Release();
        g_device = nullptr;
        return false;
    }

    g_hwnd = desc.OutputWindow;
    g_swap = swap;

    if (!CreateRtv(swap)) LogE("sin render target, overlay no dibuja");

    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        LogE("backends de ImGui no inicializaron");
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ReleaseRtv();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_hwnd = nullptr;
        g_swap = nullptr;
        return false;
    }

    if (config::Get().hookWndProc) {
        const LONG_PTR prev = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc));
        if (prev) {
            o_wndproc = reinterpret_cast<WNDPROC>(prev);
        } else {
            o_wndproc = nullptr;
            LogE("SetWindowLongPtrW fallo (%lu): se sigue sin WndProc", GetLastError());
        }
    } else {
        Log("WndProc sin enganchar (wndproc=0)");
    }

    g_autoShowAt = config::Get().autoOpen ? GetTickCount64() + 1500 : 0;
    g_nextSetupAt = 0;
    g_faults = 0;
    g_ready = true;
    Log("overlay listo, tecla del menu: 0x%02X", PrimaryKey());
    return true;
}

void DropDevice() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    ReleaseRtv();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_hwnd = nullptr;
    g_swap = nullptr;
    g_ready = false;
}

void Disarm(const char* why) {
    LogE("overlay desarmado: %s", why);
    g_dead = true;
    if (g_armed) {
        RestoreSlot(g_vtable, kIdxPresent, reinterpret_cast<void*>(o_present));
        RestoreSlot(g_vtable, kIdxResize, reinterpret_cast<void*>(o_resize));
        RestoreSlot(g_vtable, kIdxPresent1, reinterpret_cast<void*>(o_present1));
        g_armed = false;
    }
    __try {
        if (g_hwnd && o_wndproc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(o_wndproc));
        o_wndproc = nullptr;
        DropDevice();
    } __except (EXCEPTION_EXECUTE_HANDLER) { LogE("excepcion desarmando"); }
}

void DrawFrameUnsafe() {
    Msg m;
    while (Dequeue(m)) {
        if (ImGui::GetCurrentContext())
            ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.wp, m.lp);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (native::ready()) native::render();
    if (g_visible) menu::Render();

    ImGui::Render();

    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void DrawFrame() {
    if (!g_context) return;
    try {
        DrawFrameUnsafe();
    } catch (const std::exception& e) {
        LogE("excepcion en el frame: %s", e.what());
        if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope)
            ImGui::EndFrame();
    }
}

HRESULT __stdcall hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags);
HRESULT __stdcall hkPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p);

void PrepareAndDraw(IDXGISwapChain* swap) {
    if (!g_ready) {
        const ULONGLONG now = GetTickCount64();
        if (now < g_nextSetupAt) return;
        if (!Setup(swap)) {
            g_nextSetupAt = now + 500;
            return;
        }
    }
    if (swap != g_swap) {
        static bool saidSwap = false;
        if (!saidSwap) { saidSwap = true; Log("present de otro swapchain, se ignora"); }
        return;
    }
    if (!g_rtv && !CreateRtv(swap)) {
        static bool saidRtv = false;
        if (!saidRtv) { saidRtv = true; LogE("sin render target en Present, no se dibuja"); }
        return;
    }

    if (!g_loggedFirst) { g_loggedFirst = true; Log("primer Present recibido"); }
    ++g_frames;

    if (g_armed && g_frames % 120 == 0) {
        ReassertSlot(g_vtable, kIdxPresent, reinterpret_cast<void*>(&hkPresent));
        if (o_present1) ReassertSlot(g_vtable, kIdxPresent1, reinterpret_cast<void*>(&hkPresent1));
        if (config::Get().hookWndProc && g_hwnd && o_wndproc &&
            GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(hkWndProc))
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc));
        Log("frames=%llu visible=%d", g_frames, g_visible ? 1 : 0);
    }

    PollKeys();
    native::tick();

    if (g_visible) {
        // GTA recentra el cursor cada frame en su update: se le devuelve
        // nuestra posicion para que el menu sea usable (solo con foco).
        ClipCursor(nullptr);
        if (GetForegroundWindow() == g_hwnd)
            SetCursorPos(g_lastMouse.x, g_lastMouse.y);
    }

    if (!g_visible && g_autoShowAt && GetTickCount64() > g_autoShowAt) {
        g_autoShowAt = 0;
        ApplyVisible(true);
        Log("apertura automatica de diagnostico");
    }

    if (!g_visible && !native::ready()) return;

    D3DState state{};
    __try {
        SaveState(state);
        DrawFrame();
    } __finally {
        RestoreState(state);
    }
}

HRESULT __stdcall hkResize(IDXGISwapChain* swap, UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) {
    if (!o_resize) return E_FAIL;
    if (g_shuttingDown || g_dead || !g_ready || swap != g_swap) return o_resize(swap, a, b, c, d, e);

    ReleaseRtv();
    if (ImGui::GetCurrentContext()) ImGui_ImplDX11_InvalidateDeviceObjects();

    const HRESULT hr = o_resize(swap, a, b, c, d, e);

    if (SUCCEEDED(hr)) {
        CreateRtv(swap);
        if (ImGui::GetCurrentContext()) ImGui_ImplDX11_CreateDeviceObjects();
    }
    return hr;
}

void OnDeviceLost(HRESULT hr) {
    LogE("dispositivo perdido (0x%08X), overlay se reinicia", hr);
    DropDevice();
}

HRESULT __stdcall hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!o_present) return S_OK;
    if (g_shuttingDown || g_inPresent) return o_present(swap, sync, flags);
    if (g_dead) return o_present(swap, sync, flags);

    g_inPresent = true;

    __try {
        PrepareAndDraw(swap);
        g_faults = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        if (g_faults <= 10 || (g_faults % 200) == 0)
            LogE("excepcion dentro de Present (total %d)", g_faults);
        if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope)
            ImGui::EndFrame();
        if (g_faults >= kFaultLimit) Disarm("fallos seguidos en Present");
    }

    const HRESULT hr = o_present(swap, sync, flags);
    g_inPresent = false;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) OnDeviceLost(hr);
    return hr;
}

HRESULT __stdcall hkPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!o_present1) return S_OK;
    if (g_shuttingDown || g_inPresent) return o_present1(swap, sync, flags, p);
    if (g_dead) return o_present1(swap, sync, flags, p);

    g_inPresent = true;

    __try {
        PrepareAndDraw(swap);
        g_faults = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        if (g_faults <= 10 || (g_faults % 200) == 0)
            LogE("excepcion dentro de Present1 (total %d)", g_faults);
        if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope)
            ImGui::EndFrame();
        if (g_faults >= kFaultLimit) Disarm("fallos seguidos en Present1");
    }

    const HRESULT hr = o_present1(swap, sync, flags, p);
    g_inPresent = false;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) OnDeviceLost(hr);
    return hr;
}

void** GrabVtable() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"cheat_swap_host";
    RegisterClassExW(&wc);

    HWND host = CreateWindowExW(0, wc.lpszClassName, L"host", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) { LogE("sin ventana host"); return nullptr; }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = host;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL obtained{};

    void** vt = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &desc, &swap,
                                               &device, &obtained, &context);
    if (FAILED(hr) || !swap) {
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                           levels, 2, D3D11_SDK_VERSION, &desc, &swap,
                                           &device, &obtained, &context);
    }

    if (SUCCEEDED(hr) && swap) vt = *reinterpret_cast<void***>(swap);
    else LogE("D3D11CreateDeviceAndSwapChain fallo: 0x%08X", hr);

    if (swap) swap->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(host);

    return vt;
}

bool CreateSwapChainHook() {
    if (g_armed) return true;

    g_vtable = GrabVtable();
    if (!g_vtable) { LogE("no se pudo obtener la vtable de Present"); return false; }

    const bool okP  = PatchSlot(g_vtable, kIdxPresent, reinterpret_cast<void*>(&hkPresent),
                                reinterpret_cast<void**>(&o_present));
    const bool okR  = PatchSlot(g_vtable, kIdxResize, reinterpret_cast<void*>(&hkResize),
                                reinterpret_cast<void**>(&o_resize));
    const bool okP1 = (g_vtable[kIdxPresent1] && g_vtable[kIdxPresent1] != g_vtable[kIdxPresent])
                        ? PatchSlot(g_vtable, kIdxPresent1, reinterpret_cast<void*>(&hkPresent1),
                                    reinterpret_cast<void**>(&o_present1))
                        : false;

    if (!okP) { LogE("no se pudo parchear Present"); return false; }
    if (!okR) LogE("no se pudo parchear ResizeBuffers");

    g_dead = false;
    g_armed = true;
    Log("vtable parcheada (Present%s + ResizeBuffers)", okP1 ? " + Present1" : "");
    Log("originales: present=%p present1=%p", reinterpret_cast<void*>(o_present),
        reinterpret_cast<void*>(o_present1));
    return true;
}
} // namespace

namespace overlay {

bool init() {
    if (g_armed) return true;
    if (!config::Get().overlayEnabled) Log("aviso: overlayEnabled = 0 en la config");
    return CreateSwapChainHook();
}

void shutdown() {
    if (!g_armed && !g_ready) return;

    g_shuttingDown = true;
    for (int i = 0; i < 200 && g_inPresent; ++i) Sleep(5);

    if (g_armed) {
        RestoreSlot(g_vtable, kIdxPresent, reinterpret_cast<void*>(o_present));
        RestoreSlot(g_vtable, kIdxResize, reinterpret_cast<void*>(o_resize));
        RestoreSlot(g_vtable, kIdxPresent1, reinterpret_cast<void*>(o_present1));
        g_armed = false;
    }

    if (g_ready) {
        if (g_hwnd && o_wndproc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(o_wndproc));
        o_wndproc = nullptr;
        DropDevice();
    }
}

bool visible() { return g_visible; }
void setVisible(bool s) { g_toggleReq = s != g_visible.load(); }
void toggle() { g_toggleReq = true; }
bool armed() { return g_armed; }
} // namespace overlay