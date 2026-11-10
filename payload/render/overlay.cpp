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
using Present_t   = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuf_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

Present_t   o_present = nullptr;
ResizeBuf_t o_resize  = nullptr;
WNDPROC     o_wndproc = nullptr;
void**      g_vtable  = nullptr;

ID3D11Device*           g_device  = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv     = nullptr;
HWND                    g_hwnd    = nullptr;

std::atomic<bool> g_visible{ false };
std::atomic<bool> g_toggleReq{ false };
volatile bool     g_ready        = false;
volatile bool     g_armed        = false;
volatile bool     g_inPresent    = false;
volatile bool     g_shuttingDown = false;
bool              g_loggedFirst  = false;
ULONGLONG         g_frames       = 0;
ULONGLONG         g_autoShowAt   = 0;
int               g_faults       = 0;

RECT g_savedClip{};
bool g_hadClip = false;

int PrimaryKey() {
    const int k = config::Get().menuKey;
    return k ? k : VK_INSERT;
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

void CreateRtv(IDXGISwapChain* swap) {
    ReleaseRtv();
    if (!g_device || !swap) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void ApplyVisible(bool state) {
    if (state == g_visible) return;
    g_visible = state;
    if (state) {
        g_hadClip = GetClipCursor(&g_savedClip) != 0;
        ClipCursor(nullptr);
        ImGui::GetIO().MouseDrawCursor = true;
    } else {
        if (g_hadClip) ClipCursor(&g_savedClip);
        g_hadClip = false;
        ImGui::GetIO().MouseDrawCursor = false;
    }
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
    default: break;
    }
}

void PollKeys() {
    if (g_toggleReq.exchange(false)) ApplyVisible(!g_visible);

    // Insert siempre responde, aunque la config tenga otra tecla o este corrupta.
    static const int keys[] = { VK_INSERT, VK_F8, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5 };
    static bool down[sizeof(keys) / sizeof(keys[0])]{};
    static bool extraDown = false;

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const bool d = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (d && !down[i]) HandleKey(keys[i]);
        down[i] = d;
    }

    const int extra = PrimaryKey();
    if (extra && extra != VK_INSERT) {
        const bool d = (GetAsyncKeyState(extra) & 0x8000) != 0;
        if (d && !extraDown) HandleKey(extra);
        extraDown = d;
    }
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_visible) {
        Enqueue(msg, wp, lp);
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_CHAR: case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_INPUT:
            return 0;
        default:
            break;
        }
    }
    return CallWindowProcW(o_wndproc, hwnd, msg, wp, lp);
}

HRESULT ResizeBody(IDXGISwapChain* swap, UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e, bool& done) {
    if (!o_resize) return E_FAIL;

    if (g_ready && g_device) {
        ReleaseRtv();
        if (ImGui::GetCurrentContext()) ImGui_ImplDX11_InvalidateDeviceObjects();
        HRESULT hr = o_resize(swap, a, b, c, d, e);
        done = true;
        if (SUCCEEDED(hr) && g_device) {
            CreateRtv(swap);
            if (ImGui::GetCurrentContext()) ImGui_ImplDX11_CreateDeviceObjects();
        }
        return hr;
    }

    HRESULT hr = o_resize(swap, a, b, c, d, e);
    done = true;
    return hr;
}

HRESULT __stdcall hkResize(IDXGISwapChain* swap, UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) {
    if (g_shuttingDown || !o_resize) return o_resize ? o_resize(swap, a, b, c, d, e) : E_FAIL;

    bool done = false;
    HRESULT hr = E_FAIL;
    __try {
        hr = ResizeBody(swap, a, b, c, d, e, done);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogE("excepcion en ResizeBuffers");
        if (!done && o_resize) hr = o_resize(swap, a, b, c, d, e);
    }
    return hr;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags);

inline HRESULT Passthrough(IDXGISwapChain* swap, UINT sync, UINT flags, bool& done) {
    done = true;
    return o_present(swap, sync, flags);
}

// Sin __try aqui dentro: este cuerpo puede tener objetos con destructor, y las
// excepciones C++ se recogen con try/catch, no con __except.
HRESULT PresentBody(IDXGISwapChain* swap, UINT sync, UINT flags, bool& done) {
    if (!g_ready) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swap->GetDesc(&desc))) return Passthrough(swap, sync, flags, done);

        DWORD owner = 0;
        GetWindowThreadProcessId(desc.OutputWindow, &owner);
        if (owner != GetCurrentProcessId()) return Passthrough(swap, sync, flags, done);

        ID3D11Device* dev = nullptr;
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev)
            return Passthrough(swap, sync, flags, done);

        g_device = dev;
        g_device->GetImmediateContext(&g_context);
        g_hwnd = desc.OutputWindow;

        CreateRtv(swap);

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
            return Passthrough(swap, sync, flags, done);
        }

        o_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));

        g_autoShowAt = GetTickCount64() + 3000;
        g_ready = true;
        g_faults = 0;
        Log("overlay listo, tecla del menu: 0x%02X", PrimaryKey());
    }

    if (!g_loggedFirst) { g_loggedFirst = true; Log("primer Present recibido"); }
    ++g_frames;

    if (g_armed && g_frames % 120 == 0) {
        void* cur = g_vtable[8];
        if (cur != reinterpret_cast<void*>(&hkPresent)) {
            if (cur) o_present = reinterpret_cast<Present_t>(cur);
            PatchSlot(g_vtable, 8, reinterpret_cast<void*>(&hkPresent), nullptr);
        }
        if (g_hwnd && GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(hkWndProc))
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc));
    }

    PollKeys();
    if (native::ready()) native::tick();

    if (!g_visible && g_autoShowAt && GetTickCount64() > g_autoShowAt) {
        g_autoShowAt = 0;
        ApplyVisible(true);
        Log("apertura automatica de diagnostico");
    }

    D3DState state{};
    const bool draw = g_visible && g_rtv;
    if (draw) SaveState(state);

    bool rendered = false;
    try {
        Msg m;
        while (Dequeue(m)) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.wp, m.lp);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (native::ready()) native::render();
        if (g_visible) menu::Render();

        ImGui::Render();
        rendered = true;

        if (draw) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            RestoreState(state);
        } else {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } catch (const std::exception& e) {
        LogE("excepcion en el frame: %s", e.what());
        if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope)
            ImGui::EndFrame();
        if (draw) RestoreState(state);
    }

    if (!rendered) {
        const HRESULT hr0 = o_present(swap, sync, flags);
        done = true;
        return hr0;
    }

    const HRESULT hr = o_present(swap, sync, flags);
    done = true;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LogE("dispositivo perdido (0x%08X), overlay se reinicia", hr);
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        ReleaseRtv();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_ready = false;
    }
    return hr;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (g_shuttingDown || g_inPresent || !o_present) return o_present(swap, sync, flags);

    g_inPresent = true;
    HRESULT hr = S_OK;
    bool done = false;

    __try {
        hr = PresentBody(swap, sync, flags, done);
        g_faults = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_faults;
        if (g_faults <= 10 || (g_faults % 200) == 0)
            LogE("excepcion dentro de Present (total %d)", g_faults);
        if (!done) hr = o_present(swap, sync, flags);
    }

    g_inPresent = false;
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
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = host;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL obtained{};

    void** vt = nullptr;

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                     levels, 2, D3D11_SDK_VERSION, &desc, &swap,
                                                     &device, &obtained, &context);
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

    const bool okP = PatchSlot(g_vtable, 8, reinterpret_cast<void*>(&hkPresent),
                               reinterpret_cast<void**>(&o_present));
    PatchSlot(g_vtable, 13, reinterpret_cast<void*>(&hkResize), reinterpret_cast<void**>(&o_resize));

    if (!okP) { LogE("no se pudo parchear Present"); return false; }

    g_armed = true;
    Log("vtable parcheada (Present + ResizeBuffers)");
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
        RestoreSlot(g_vtable, 8, reinterpret_cast<void*>(o_present));
        RestoreSlot(g_vtable, 13, reinterpret_cast<void*>(o_resize));
        g_armed = false;
    }

    if (g_ready) {
        if (g_hwnd && o_wndproc)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(o_wndproc));
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ReleaseRtv();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_ready = false;
    }
}

bool visible() { return g_visible; }
void setVisible(bool s) { g_toggleReq = s != g_visible.load(); }
void toggle() { g_toggleReq = true; }
bool armed() { return g_armed; }
} // namespace overlay