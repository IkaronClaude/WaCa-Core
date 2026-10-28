// Swap chain fallback: when DXGI refuses the swap chain the engine asks for, retry with simpler settings.
//
// Machines with a limited display adapter - a VM's remote display with the WARP software renderer, which is how the
// mod is tested without a GPU - refuse the game's swap chain ("D3D12Viewport.cpp:572 E_INVALIDARG") and the game
// dies at start-up. The engine's request always goes through first, unchanged: a swap chain that works is never
// touched, so on a real GPU this does nothing. Only on a failure do we retry, step by step simpler (no tearing /
// waitable flags, stretch scaling, no alpha, an 8-bit back buffer, two buffers, windowed), and log every attempt.
// Hooked: IDXGIFactory2::CreateSwapChainForHwnd (vtable slot 15), IDXGIFactory::CreateSwapChain (slot 10) and, on
// the first swap chain made, IDXGISwapChain::ResizeBuffers (slot 13).
#include "render.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>

#include <string>

#include "MinHook.h"
#include "log.h"

namespace {

using ForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using LegacyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);

ForHwndFn g_forHwnd = nullptr;
LegacyFn g_legacy = nullptr;

void LogDesc(const wchar_t* what, const DXGI_SWAP_CHAIN_DESC1& d, bool fullscreenDesc, HRESULT hr) {
    logx::Line(L"[render] %s: %ux%u format %d buffers %u scaling %d effect %d alpha %d flags 0x%X fullscreen-desc %d -> 0x%08X",
               what, d.Width, d.Height, d.Format, d.BufferCount, d.Scaling, d.SwapEffect, d.AlphaMode, d.Flags,
               fullscreenDesc ? 1 : 0, static_cast<unsigned>(hr));
}

// IDXGISwapChain::ResizeBuffers (vtable slot 13): the engine resizes right after creating the swap chain, and on the
// VM that is what fails ("D3D12Viewport.cpp:572 ... DXGIFormat=0x18, AllowTearing=1", 2026-10-25)
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ResizeFn g_resize = nullptr;

HRESULT STDMETHODCALLTYPE ResizeDetour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    HRESULT hr = g_resize(sc, count, w, h, format, flags);
    if (SUCCEEDED(hr)) {
        logx::Line(L"[render] resize: %u buffers %ux%u format %d flags 0x%X", count, w, h, format, flags);
        return hr;
    }
    logx::Line(L"[render] resize refused: %u buffers %ux%u format %d flags 0x%X -> 0x%08X", count, w, h, format, flags,
               static_cast<unsigned>(hr));
    struct Try { UINT count; DXGI_FORMAT format; UINT flags; };
    const Try tries[] = {
        {count, DXGI_FORMAT_UNKNOWN, flags},                                    // keep the swap chain's own format
        {count, DXGI_FORMAT_R8G8B8A8_UNORM, flags},
        {0, DXGI_FORMAT_UNKNOWN, flags},                                        // keep its buffer count too
        {count, format, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING},
        {0, DXGI_FORMAT_UNKNOWN, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING},
    };
    for (const auto& t : tries) {
        hr = g_resize(sc, t.count, w, h, t.format, t.flags);
        logx::Line(L"[render] resize retry: %u buffers format %d flags 0x%X -> 0x%08X", t.count, t.format, t.flags,
                   static_cast<unsigned>(hr));
        if (SUCCEEDED(hr)) return hr;
    }
    return hr;
}

// IDXGISwapChain3::ResizeBuffers1 (vtable slot 39): what the engine's D3D12 viewport actually calls (with its present
// queues); plain ResizeBuffers is never reached there
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*);
Resize1Fn g_resize1 = nullptr;

HRESULT STDMETHODCALLTYPE Resize1Detour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                                        const UINT* nodeMasks, IUnknown* const* queues) {
    HRESULT hr = g_resize1(sc, count, w, h, format, flags, nodeMasks, queues);
    if (SUCCEEDED(hr)) {
        logx::Line(L"[render] resize1: %u buffers %ux%u format %d flags 0x%X", count, w, h, format, flags);
        return hr;
    }
    logx::Line(L"[render] resize1 refused: %u buffers %ux%u format %d flags 0x%X -> 0x%08X", count, w, h, format, flags,
               static_cast<unsigned>(hr));
    struct Try { UINT count; DXGI_FORMAT format; UINT flags; bool queues; };
    const Try tries[] = {
        {count, DXGI_FORMAT_UNKNOWN, flags, true},                               // keep the swap chain's own format
        {count, format, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, true},
        {count, format, flags, false},                                            // as a plain ResizeBuffers
        {count, DXGI_FORMAT_UNKNOWN, flags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, false},
        {0, DXGI_FORMAT_UNKNOWN, flags, false},
    };
    for (const auto& t : tries) {
        hr = t.queues ? g_resize1(sc, t.count, w, h, t.format, t.flags, nodeMasks, queues)
                      : (g_resize ? g_resize(sc, t.count, w, h, t.format, t.flags)
                                  : reinterpret_cast<ResizeFn>((*reinterpret_cast<void***>(sc))[13])(sc, t.count, w, h, t.format, t.flags));
        logx::Line(L"[render] resize1 retry (%s): %u buffers format %d flags 0x%X -> 0x%08X",
                   t.queues ? L"with queues" : L"plain", t.count, t.format, t.flags, static_cast<unsigned>(hr));
        if (SUCCEEDED(hr)) return hr;
    }
    return hr;
}

// IDXGISwapChain::Present (vtable slot 8): the call that actually fails on the VM - the engine's viewport presents,
// gets E_INVALIDARG five times and dies (read from the game's code at 0x14350DCF3, 2026-10-25). Retried without the
// tearing flag, then without any flags.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present = nullptr;

// the debug layers' stored messages (they only print to an attached debugger), into this log - after a failure
void DumpDebugMessages() {
    using GetInterfaceFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    HMODULE dbg = GetModuleHandleW(L"dxgidebug.dll");
    if (!dbg) dbg = GetModuleHandleW(L"dxgi.dll");
    auto get = dbg ? reinterpret_cast<GetInterfaceFn>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "DXGIGetDebugInterface1")) : nullptr;
    IDXGIInfoQueue* queue = nullptr;
    if (!get || FAILED(get(0, __uuidof(IDXGIInfoQueue), reinterpret_cast<void**>(&queue))) || !queue) {
        logx::Line(L"[render] no debug message queue (start with -d3ddebug)");
        return;
    }
    UINT64 n = queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    logx::Line(L"[render] %llu debug messages stored", n);
    for (UINT64 i = (n > 30 ? n - 30 : 0); i < n; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &size);
        std::string buf(size, '\0');
        auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
        if (SUCCEEDED(queue->GetMessage(DXGI_DEBUG_ALL, i, msg, &size)) && msg->pDescription) {
            int len = MultiByteToWideChar(CP_ACP, 0, msg->pDescription, -1, nullptr, 0);
            std::wstring w(len > 0 ? len - 1 : 0, L'\0');
            if (len > 1) MultiByteToWideChar(CP_ACP, 0, msg->pDescription, -1, w.data(), len);
            logx::Line(L"[render]   [%d] %s", msg->Severity, w.c_str());
        }
    }
    queue->Release();
}

// the D3D12 debug layer's stored messages (on the device behind the swap chain's queue)
void DumpD3D12Messages(IDXGISwapChain* sc) {
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12InfoQueue* info = nullptr;
    HRESULT hq = sc->GetDevice(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    HRESULT hd = queue ? queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device)) : E_POINTER;
    if (device) logx::Line(L"[render] device removed reason 0x%08X", static_cast<unsigned>(device->GetDeviceRemovedReason()));
    HRESULT hi = device ? device->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&info)) : E_POINTER;
    if (!info) {
        logx::Line(L"[render] no D3D12 message queue: queue 0x%08X device 0x%08X info queue 0x%08X (start with -d3ddebug)",
                   static_cast<unsigned>(hq), static_cast<unsigned>(hd), static_cast<unsigned>(hi));
    } else {
        UINT64 n = info->GetNumStoredMessages();
        logx::Line(L"[render] %llu D3D12 messages stored", n);
        for (UINT64 i = (n > 30 ? n - 30 : 0); i < n; ++i) {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::string buf(size, '\0');
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info->GetMessage(i, msg, &size)) && msg->pDescription) {
                int len = MultiByteToWideChar(CP_ACP, 0, msg->pDescription, -1, nullptr, 0);
                std::wstring w(len > 0 ? len - 1 : 0, L'\0');
                if (len > 1) MultiByteToWideChar(CP_ACP, 0, msg->pDescription, -1, w.data(), len);
                logx::Line(L"[render]   d3d12 [%d] %s", msg->Severity, w.c_str());
            }
        }
    }
    if (info) info->Release();
    if (device) device->Release();
    if (queue) queue->Release();
}

// -warp -wacaoffscreen: frames are rendered but never presented. The VM's display path refuses presents after a few
// frames and took DWM down once; without presents DWM is never involved. The game runs on; screenshots (the engine's
// "shot" command) read the rendered frame, not the window, and input posted to the window still arrives.
bool g_offscreen = false;

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_offscreen) {
        // the frame rate, every 10 s of frames (on WARP it is the first thing to know)
        static ULONGLONG since = GetTickCount64();
        static unsigned frames = 0, total = 0;
        ++frames;
        ++total;
        ULONGLONG now = GetTickCount64();
        if (total == 1) logx::Line(L"[render] offscreen: frames are rendered, not presented");
        if (now - since >= 10000) {
            logx::Line(L"[render] offscreen: %.1f fps (%u frames so far)", frames * 1000.0 / (now - since), total);
            since = now;
            frames = 0;
        }
        return S_OK;
    }
    HRESULT hr = g_present(sc, sync, flags);
    if (SUCCEEDED(hr) || hr != DXGI_ERROR_INVALID_CALL && hr != E_INVALIDARG) return hr;
    HRESULT hr2 = g_present(sc, sync, flags & ~DXGI_PRESENT_ALLOW_TEARING);
    HRESULT hr3 = S_OK;
    if (FAILED(hr2)) hr3 = g_present(sc, 1, 0);
    static int logged = 0;
    if (logged < 10) {
        ++logged;
        logx::Line(L"[render] present refused (%d): sync %u flags 0x%X -> 0x%08X; no tearing -> 0x%08X; plain -> 0x%08X",
                   logged, sync, flags, static_cast<unsigned>(hr), static_cast<unsigned>(hr2), static_cast<unsigned>(hr3));
        if (logged == 1) DumpDebugMessages();
        static bool d3d12Dumped = false;
        if (FAILED(hr2) && FAILED(hr3) && !d3d12Dumped) {
            d3d12Dumped = true;
            DumpD3D12Messages(sc);
        }
    }
    return FAILED(hr2) ? hr3 : hr2;
}

// IDXGISwapChain::SetFullscreenState (vtable slot 10): logged
using FullscreenFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
FullscreenFn g_fullscreen = nullptr;

HRESULT STDMETHODCALLTYPE FullscreenDetour(IDXGISwapChain* sc, BOOL fullscreen, IDXGIOutput* output) {
    HRESULT hr = g_fullscreen(sc, fullscreen, output);
    logx::Line(L"[render] fullscreen state %d -> 0x%08X", fullscreen, static_cast<unsigned>(hr));
    return hr;
}

void HookResize(IDXGISwapChain1* sc) {
    if (g_resize || !sc) return;
    void** vtable = *reinterpret_cast<void***>(sc);
    bool presentOk = MH_CreateHook(vtable[8], reinterpret_cast<void*>(&PresentDetour), reinterpret_cast<void**>(&g_present)) == MH_OK &&
                     MH_EnableHook(vtable[8]) == MH_OK;
    logx::Line(L"[render] present fallback %s", presentOk ? L"ready" : L"could not hook Present");
    if (MH_CreateHook(vtable[10], reinterpret_cast<void*>(&FullscreenDetour), reinterpret_cast<void**>(&g_fullscreen)) == MH_OK)
        MH_EnableHook(vtable[10]);
    bool ok = MH_CreateHook(vtable[13], reinterpret_cast<void*>(&ResizeDetour), reinterpret_cast<void**>(&g_resize)) == MH_OK &&
              MH_EnableHook(vtable[13]) == MH_OK;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3) {
        void** vt3 = *reinterpret_cast<void***>(sc3);
        ok = MH_CreateHook(vt3[39], reinterpret_cast<void*>(&Resize1Detour), reinterpret_cast<void**>(&g_resize1)) == MH_OK &&
             MH_EnableHook(vt3[39]) == MH_OK && ok;
        sc3->Release();
    }
    logx::Line(L"[render] resize fallback %s", ok ? L"ready" : L"could not hook ResizeBuffers / ResizeBuffers1");
}

HRESULT STDMETHODCALLTYPE ForHwndDetour(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                                        const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                        IDXGIOutput* output, IDXGISwapChain1** swapChain) {
    // on the software renderer (-warp: a VM / test machine) the swap chain is made without tearing: its display path
    // rejects every Present of a tearing swap chain (E_INVALIDARG, nothing from the debug layers - 2026-10-25)
    DXGI_SWAP_CHAIN_DESC1 warpDesc{};
    if (desc && wcsstr(GetCommandLineW(), L"-warp")) {
        warpDesc = *desc;
        warpDesc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        desc = &warpDesc;
    }
    HRESULT hr = g_forHwnd(factory, device, hwnd, desc, fs, output, swapChain);
    if (SUCCEEDED(hr)) {
        if (desc) LogDesc(L"swap chain created", *desc, fs != nullptr, hr);
        if (swapChain) HookResize(*swapChain);
        return hr;
    }
    if (!desc) return hr;
    LogDesc(L"swap chain refused", *desc, fs != nullptr, hr);
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f = fs;
    for (int step = 1; step <= 5; ++step) {
        switch (step) {
            case 1: d.Flags &= ~(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT); break;
            case 2: d.Scaling = DXGI_SCALING_STRETCH; d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED; d.Stereo = FALSE; break;
            case 3: d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case 4: d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.SampleDesc = {1, 0}; break;
            case 5: f = nullptr; output = nullptr; break;
        }
        hr = g_forHwnd(factory, device, hwnd, &d, f, output, swapChain);
        wchar_t what[32];
        swprintf_s(what, L"retry %d", step);
        LogDesc(what, d, f != nullptr, hr);
        if (SUCCEEDED(hr)) {
            HookResize(*swapChain);
            return hr;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE LegacyDetour(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                       IDXGISwapChain** swapChain) {
    HRESULT hr = g_legacy(factory, device, desc, swapChain);
    if (SUCCEEDED(hr) || !desc) return hr;
    logx::Line(L"[render] swap chain (legacy) refused: %ux%u format %d buffers %u effect %d flags 0x%X -> 0x%08X",
               desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->BufferCount,
               desc->SwapEffect, desc->Flags, static_cast<unsigned>(hr));
    DXGI_SWAP_CHAIN_DESC d = *desc;
    d.Flags &= ~(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.SampleDesc = {1, 0};
    d.Windowed = TRUE;
    hr = g_legacy(factory, device, &d, swapChain);
    logx::Line(L"[render] legacy retry -> 0x%08X", static_cast<unsigned>(hr));
    return hr;
}

// With -d3ddebug the D3D12 / DXGI debug layers explain their errors - through OutputDebugString, and a shipping build
// keeps no log file: their messages are copied into WaCaNative.log ("[debug]").
using OdsAFn = void(WINAPI*)(LPCSTR);
using OdsWFn = void(WINAPI*)(LPCWSTR);
OdsAFn g_odsA = nullptr;
OdsWFn g_odsW = nullptr;

void WINAPI OdsADetour(LPCSTR text) {
    if (text) {
        int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_ACP, 0, text, -1, w.data(), n);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        logx::Line(L"[debug] %s", w.c_str());
    }
    g_odsA(text);
}

void WINAPI OdsWDetour(LPCWSTR text) {
    if (text) {
        std::wstring w(text);
        while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        logx::Line(L"[debug] %s", w.c_str());
    }
    g_odsW(text);
}

void CaptureDebugOutput() {
    HMODULE k32 = GetModuleHandleW(L"kernelbase.dll");
    if (!k32) k32 = GetModuleHandleW(L"kernel32.dll");
    void* a = reinterpret_cast<void*>(GetProcAddress(k32, "OutputDebugStringA"));
    void* w = reinterpret_cast<void*>(GetProcAddress(k32, "OutputDebugStringW"));
    bool ok = a && w &&
              MH_CreateHook(a, reinterpret_cast<void*>(&OdsADetour), reinterpret_cast<void**>(&g_odsA)) == MH_OK &&
              MH_CreateHook(w, reinterpret_cast<void*>(&OdsWDetour), reinterpret_cast<void**>(&g_odsW)) == MH_OK &&
              MH_EnableHook(a) == MH_OK && MH_EnableHook(w) == MH_OK;
    logx::Line(L"[render] -d3ddebug: debug output %s", ok ? L"copied to this log" : L"could not be captured");
}

// A shipping build ignores -d3ddebug, so the debug layers are switched on here, before the engine makes its device:
// D3D12's (d3d12SDKLayers.dll, shipped with the game) and DXGI's (factories made with the debug flag, when the
// Windows graphics tools' DXGIDebug.dll is installed).
using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactory2Fn g_createFactory2 = nullptr;

HRESULT WINAPI CreateFactory2Detour(UINT flags, REFIID riid, void** factory) {
    HRESULT hr = g_createFactory2(flags | DXGI_CREATE_FACTORY_DEBUG, riid, factory);
    if (FAILED(hr)) hr = g_createFactory2(flags, riid, factory);
    return hr;
}

void EnableDebugLayers() {
    using GetDebugFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto getDebug = d3d12 ? reinterpret_cast<GetDebugFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ID3D12Debug* debug = nullptr;
    if (getDebug && SUCCEEDED(getDebug(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug))) && debug) {
        debug->EnableDebugLayer();
        debug->Release();
        logx::Line(L"[render] -d3ddebug: D3D12 debug layer on");
    } else {
        logx::Line(L"[render] -d3ddebug: no D3D12 debug layer");
    }
    if (LoadLibraryW(L"dxgidebug.dll")) {
        void* create = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory2"));
        bool ok = create && MH_CreateHook(create, reinterpret_cast<void*>(&CreateFactory2Detour),
                                          reinterpret_cast<void**>(&g_createFactory2)) == MH_OK &&
                  MH_EnableHook(create) == MH_OK;
        logx::Line(L"[render] -d3ddebug: DXGI debug factories %s", ok ? L"on" : L"could not be hooked");
    } else {
        logx::Line(L"[render] -d3ddebug: no DXGIDebug.dll (Windows graphics tools) - DXGI debug messages off");
    }
}

}  // namespace

namespace render {

void Install() {
    // Only on the software renderer (-warp): a test machine without a GPU. Players are never hooked here.
    // STATUS 2026-10-25, unfinished: with the tearing flag kept off the VM's swap chain, ResizeBuffers1 and the first
    // five Presents succeed; from the sixth on every Present returns E_INVALIDARG (DXGI's debug layer silent), and one
    // run took the VM's DWM down with it. Experiments with rendering on the VM are paused.
    if (!wcsstr(GetCommandLineW(), L"-warp")) return;
    g_offscreen = wcsstr(GetCommandLineW(), L"-wacaoffscreen") != nullptr;
    if (wcsstr(GetCommandLineW(), L"-d3ddebug")) {
        CaptureDebugOutput();
        EnableDebugLayers();
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))) || !factory) {
        logx::Line(L"[render] no DXGI factory - swap chain fallback off");
        return;
    }
    void** vtable = *reinterpret_cast<void***>(factory);
    bool ok = MH_CreateHook(vtable[15], reinterpret_cast<void*>(&ForHwndDetour), reinterpret_cast<void**>(&g_forHwnd)) == MH_OK &&
              MH_EnableHook(vtable[15]) == MH_OK;
    ok = MH_CreateHook(vtable[10], reinterpret_cast<void*>(&LegacyDetour), reinterpret_cast<void**>(&g_legacy)) == MH_OK &&
         MH_EnableHook(vtable[10]) == MH_OK && ok;
    factory->Release();
    logx::Line(L"[render] swap chain fallback %s", ok ? L"ready" : L"could not hook DXGI");
}

}  // namespace render
