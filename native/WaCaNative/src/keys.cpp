// Raw keys: every key / mouse button press as it reaches the game window, as the Lua event KeyDown(nil, <Unreal key
// name>), its release as KeyUp(nil, <name>), and the mouse moving as MouseMove(nil, x, y). The engine routes input its
// own way before mods see it - a key bound to no menu row never reached the player controller's key handler, and
// while the game is paused nothing does (Escape could not close the pause menu,
// operator 2026-10-25). The game thread pumps the window messages with DispatchMessageW, so hooking it sees every
// press first, on the game thread. Nothing is changed or swallowed: the message goes on to the game as it was.
#include "keys.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "MinHook.h"
#include "bridge.h"
#include "log.h"

namespace {

using DispatchFn = LRESULT(WINAPI*)(const MSG*);
DispatchFn g_dispatch = nullptr;
// keys the game doesn't get right now (keys::Swallow) - the game thread reads, Lua (also the game thread) writes
std::atomic<bool> g_swallow[256]{};

// a Windows virtual key as an Unreal key name ("" when not mapped)
std::string KeyName(WPARAM vk) {
    switch (vk) {
        case VK_BACK: return "BackSpace";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Escape";
        case VK_SPACE: return "SpaceBar";
        case VK_LEFT: return "Left";
        case VK_UP: return "Up";
        case VK_RIGHT: return "Right";
        case VK_DOWN: return "Down";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_SHIFT: return "LeftShift";
        case VK_CONTROL: return "LeftControl";
        case VK_MENU: return "LeftAlt";
    }
    static const char* digits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    return "";
}

LRESULT WINAPI DispatchDetour(const MSG* msg) {
    if (msg && msg->message >= WM_LBUTTONDOWN && msg->message <= WM_MBUTTONUP) {
        static int seen = 0;
        if (seen < 6) {
            ++seen;
            logx::Line(L"[keys] mouse message 0x%X to %p", msg->message, msg->hwnd);
        }
    }
    if (msg) {
        const char* mouse = nullptr;
        switch (msg->message) {
            case WM_LBUTTONDOWN: mouse = "LeftMouseButton"; break;
            case WM_RBUTTONDOWN: mouse = "RightMouseButton"; break;
            case WM_MBUTTONDOWN: mouse = "MiddleMouseButton"; break;
        }
        if (msg->message == WM_MOUSEMOVE) {
            // MouseMove(nil, x, y): the cursor moved (window pixels) - at most every 40 ms (a move stream would flood
            // Lua; what hovers a menu button follows soon enough), and only when it actually moved
            static DWORD last = 0;
            static LPARAM lastPos = -1;
            if (msg->lParam != lastPos && msg->time - last >= 40) {
                last = msg->time;
                lastPos = msg->lParam;
                bridge::Emit("MouseMove", {std::to_string(static_cast<short>(LOWORD(msg->lParam))),
                                           std::to_string(static_cast<short>(HIWORD(msg->lParam)))});
            }
        } else if (mouse) {
            bridge::Emit("KeyDown", {mouse});
        } else if (msg->message == WM_LBUTTONUP || msg->message == WM_RBUTTONUP || msg->message == WM_MBUTTONUP) {
            bridge::Emit("KeyUp", {msg->message == WM_LBUTTONUP ? "LeftMouseButton"
                                   : msg->message == WM_RBUTTONUP ? "RightMouseButton" : "MiddleMouseButton"});
        } else if (msg->message == WM_MOUSEWHEEL) {
            // MouseWheel(nil, notches): + away from you, - towards you
            bridge::Emit("MouseWheel", {std::to_string(GET_WHEEL_DELTA_WPARAM(msg->wParam) / WHEEL_DELTA)});
        } else if ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) && !(msg->lParam & (1LL << 30))) {
            // a fresh press (bit 30: the key was already down - auto-repeat)
            std::string name = KeyName(msg->wParam);
            if (!name.empty()) bridge::Emit("KeyDown", {name});
        } else if (msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP) {
            // KeyUp(nil, name): the key is let go - after its typed character went by (a text box focused on a key's
            // press got that same key's Enter and committed at once: VM 2026-10-28)
            std::string name = KeyName(msg->wParam);
            if (!name.empty()) bridge::Emit("KeyUp", {name});
        }
        // a swallowed key: the game never sees it - its press, release and typed character (Enter types a carriage
        // return, Tab a tab)
        WPARAM vk = 0;
        switch (msg->message) {
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: vk = msg->wParam; break;
            case WM_CHAR: vk = msg->wParam == 0x0D ? VK_RETURN : (msg->wParam == 0x09 ? VK_TAB : 0); break;
        }
        if (vk && vk < 256 && g_swallow[vk].load(std::memory_order_relaxed)) return 0;
    }
    return g_dispatch(msg);
}

}  // namespace

namespace keys {

void Install() {
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "DispatchMessageW"));
    bool ok = target && MH_CreateHook(target, reinterpret_cast<void*>(&DispatchDetour), reinterpret_cast<void**>(&g_dispatch)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    logx::Line(L"[keys] raw key events %s", ok ? L"on" : L"OFF (could not hook DispatchMessageW)");
}

}  // namespace keys

namespace keys {

void Swallow(unsigned int vk, bool on) {
    if (vk == 0 || vk >= 256) return;
    if (g_swallow[vk].exchange(on) != on) logx::Line(L"[keys] key %X %s", vk, on ? L"kept from the game" : L"back to the game");
}

void CursorPos(long& x, long& y) {
    POINT p{};
    GetCursorPos(&p);
    x = p.x;
    y = p.y;
}

}  // namespace keys
