// Reverse-engineering helpers, driven from Lua (WaCa.Command("Dev...")) - for finding the game code to hook, not
// used by any feature:
//   DevWatch|<address hex>|<length 1/2/4/8>   hardware data breakpoint (read/write) on every thread; each distinct
//                                            instruction that touches the address is logged once with its module
//                                            offset and a short call stack (WaCaNative.log, "[dev]")
//   DevUnwatch                               clears it
//   DevPressKey|<virtual key hex>            a key press + release posted to the game window, as Windows sends it
//   DevStack|<label>                         the calling thread's native call stack (from inside a Lua hook: shows
//                                            the game code that called the hooked function)
#include "dev.h"

#include <windows.h>
#include <tlhelp32.h>

#include <mutex>
#include <set>
#include <string>

#include "log.h"

namespace {

uintptr_t g_watch = 0;
std::mutex g_seenLock;
std::set<uintptr_t> g_seen;
PVOID g_handler = nullptr;

std::wstring Where(uintptr_t addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod)) {
        wchar_t buf[32];
        swprintf_s(buf, L"%p", reinterpret_cast<void*>(addr));
        return buf;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring name(path);
    name = name.substr(name.find_last_of(L"\\/") + 1);
    wchar_t buf[MAX_PATH + 32];
    // the exe's offsets are printed as the file's own addresses (image base 0x140000000) for the disassembler
    uintptr_t off = addr - reinterpret_cast<uintptr_t>(mod);
    if (mod == GetModuleHandleW(nullptr)) swprintf_s(buf, L"%s!%llX", name.c_str(), 0x140000000ull + off);
    else swprintf_s(buf, L"%s+%llX", name.c_str(), static_cast<unsigned long long>(off));
    return buf;
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    auto* ctx = info->ContextRecord;
    if (!(ctx->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 = 0;
    uintptr_t rip = ctx->Rip;
    {
        std::lock_guard g(g_seenLock);
        if (!g_seen.insert(rip).second || g_seen.size() > 200) return EXCEPTION_CONTINUE_EXECUTION;
    }
    // a short stack: unwind from the trapping context
    std::wstring stack;
    CONTEXT c = *ctx;
    for (int i = 0; i < 8 && c.Rip; ++i) {
        DWORD64 imageBase = 0;
        auto* fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fn) break;
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
        if (c.Rip) stack += L" <- " + Where(c.Rip);
    }
    logx::Line(L"[dev] watch hit at %s%s", Where(rip).c_str(), stack.c_str());
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetDebugRegisters(uintptr_t addr, int len) {
    DWORD lenBits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        bool other = te.th32ThreadID != self;
        if (other) SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addr;
            // L0 enable, RW0 = 11 (read or write), LEN0
            c.Dr7 = addr ? (1 | (3u << 16) | (lenBits << 18)) : 0;
            if (SetThreadContext(t, &c)) ++n;
        }
        if (other) ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    logx::Line(L"[dev] debug registers set on %d threads (%p, %d bytes)", n, reinterpret_cast<void*>(addr), len);
}

struct FindWindowCtx { HWND hwnd = nullptr; };
BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (pid == GetCurrentProcessId() && wcscmp(cls, L"UnrealWindow") == 0) {
        reinterpret_cast<FindWindowCtx*>(lp)->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

namespace dev {

void Watch(const std::wstring& addrHex, const std::wstring& lenText) {
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, OnException);
    {
        std::lock_guard g(g_seenLock);
        g_seen.clear();
    }
    g_watch = static_cast<uintptr_t>(std::wcstoull(addrHex.c_str(), nullptr, 16));
    int len = lenText.empty() ? 8 : _wtoi(lenText.c_str());
    SetDebugRegisters(g_watch, len);
}

void Unwatch() {
    g_watch = 0;
    SetDebugRegisters(0, 8);
}

void Stack(const std::wstring& label) {
    void* frames[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
    std::wstring out;
    for (USHORT i = 0; i < n; ++i) out += L"\n    " + Where(reinterpret_cast<uintptr_t>(frames[i]));
    logx::Line(L"[dev] stack %s:%s", label.c_str(), out.c_str());
}

// an Unreal key name ("BackSpace", "Enter", "A", "F5", "One") as a Windows virtual key, 0 when unknown
UINT VirtualKey(const std::wstring& name) {
    static const struct { const wchar_t* name; UINT vk; } kNamed[] = {
        {L"BackSpace", VK_BACK}, {L"Tab", VK_TAB}, {L"Enter", VK_RETURN}, {L"Escape", VK_ESCAPE},
        {L"SpaceBar", VK_SPACE}, {L"Left", VK_LEFT}, {L"Up", VK_UP}, {L"Right", VK_RIGHT}, {L"Down", VK_DOWN},
        {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"LeftShift", VK_LSHIFT}, {L"RightShift", VK_RSHIFT},
        {L"LeftControl", VK_LCONTROL}, {L"RightControl", VK_RCONTROL}, {L"LeftAlt", VK_LMENU}, {L"RightAlt", VK_RMENU},
        {L"Zero", '0'}, {L"One", '1'}, {L"Two", '2'}, {L"Three", '3'}, {L"Four", '4'}, {L"Five", '5'},
        {L"Six", '6'}, {L"Seven", '7'}, {L"Eight", '8'}, {L"Nine", '9'},
    };
    for (const auto& k : kNamed)
        if (name == k.name) return k.vk;
    if (name.size() == 1 && name[0] >= L'A' && name[0] <= L'Z') return name[0];
    if (name.size() >= 2 && name[0] == L'F') {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return 0;
}

void PressKeyNamed(const std::wstring& keyName) {
    UINT vk = VirtualKey(keyName);
    if (!vk) {
        logx::Line(L"[keys] PressKey: no Windows key for %s", keyName.c_str());
        return;
    }
    wchar_t hex[16];
    swprintf_s(hex, L"%X", vk);
    PressKey(hex);
}

void PressKey(const std::wstring& vkHex) {
    FindWindowCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.hwnd) {
        logx::Line(L"[dev] PressKey: no game window");
        return;
    }
    UINT vk = static_cast<UINT>(std::wcstoul(vkHex.c_str(), nullptr, 16));
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    LPARAM up = down | (1LL << 30) | (1LL << 31);
    PostMessageW(ctx.hwnd, WM_KEYDOWN, vk, down);
    PostMessageW(ctx.hwnd, WM_KEYUP, vk, up);
    logx::Line(L"[dev] PressKey %X posted to window %p", vk, ctx.hwnd);
}

}  // namespace dev
