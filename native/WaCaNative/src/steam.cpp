// Steam's on-screen keyboard for text fields on a controller (the game's own steam_api64.dll, flat API):
//   SteamKeyboard|<description>|<existing text>
// First the floating keyboard (Steam Deck / Big Picture), which types into the focused field as ordinary key presses;
// when Steam refuses it, the full-screen one, whose text arrives with GamepadTextInputDismissed_t (on the game thread:
// the engine runs Steam's callbacks there) - handed to Lua as the event SteamTextInput(submitted "1" / "0", text).
#include "steam.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "bridge.h"
#include "log.h"

namespace {

using UtilsFn = void* (*)();
using ShowFloatingFn = bool (*)(void* utils, int mode, int x, int y, int w, int h);
using ShowFullFn = bool (*)(void* utils, int inputMode, int lineMode, const char* description, uint32_t maxChars,
                            const char* existing);
using TextLengthFn = uint32_t (*)(void* utils);
using TextFn = bool (*)(void* utils, char* text, uint32_t size);
using RegisterFn = void (*)(void* callback, int id);

struct Api {
    UtilsFn utils = nullptr;
    ShowFloatingFn floating = nullptr;
    ShowFullFn full = nullptr;
    TextLengthFn length = nullptr;
    TextFn text = nullptr;
    RegisterFn registerCallback = nullptr;
} g_api;

// steam_api's CCallbackBase, declared the same way so MSVC lays out the same vtable
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t flags = 0;
    int id = 0;
};

// GamepadTextInputDismissed_t (k_iSteamUtilsCallbacks + 14)
struct Dismissed {
    bool submitted;
    uint32_t length;
    uint32_t appId;
};
constexpr int kDismissedId = 714;

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

class DismissedCallback : public CallbackBase {
public:
    void Run(void* param) override {
        auto* d = static_cast<Dismissed*>(param);
        std::string text;
        if (d->submitted && g_api.length && g_api.text) {
            uint32_t n = g_api.length(g_api.utils());
            text.resize(n + 1);
            if (g_api.text(g_api.utils(), text.data(), n + 1)) text.resize(strlen(text.c_str()));
            else text.clear();
        }
        bridge::Emit("SteamTextInput", {d->submitted ? "1" : "0", text});
        logx::Line(L"[steam] keyboard closed (%s, %zu bytes)", d->submitted ? L"submitted" : L"cancelled", text.size());
    }
    void Run(void* param, bool, uint64_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(Dismissed); }
} g_dismissed;

bool Load() {
    if (g_api.utils) return true;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) return false;
    g_api.utils = reinterpret_cast<UtilsFn>(GetProcAddress(dll, "SteamAPI_SteamUtils_v010"));
    g_api.floating = reinterpret_cast<ShowFloatingFn>(GetProcAddress(dll, "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput"));
    g_api.full = reinterpret_cast<ShowFullFn>(GetProcAddress(dll, "SteamAPI_ISteamUtils_ShowGamepadTextInput"));
    g_api.length = reinterpret_cast<TextLengthFn>(GetProcAddress(dll, "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength"));
    g_api.text = reinterpret_cast<TextFn>(GetProcAddress(dll, "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput"));
    g_api.registerCallback = reinterpret_cast<RegisterFn>(GetProcAddress(dll, "SteamAPI_RegisterCallback"));
    if (!g_api.utils || !g_api.utils()) {
        g_api.utils = nullptr;
        return false;
    }
    if (g_api.registerCallback) {
        g_dismissed.id = kDismissedId;
        g_api.registerCallback(&g_dismissed, kDismissedId);
    }
    return true;
}

}  // namespace

namespace steam {

void ShowKeyboard(const std::wstring& description, const std::wstring& existing) {
    if (!Load()) {
        logx::Line(L"[steam] keyboard: Steam is not running (no steam_api64 / SteamUtils)");
        return;
    }
    void* utils = g_api.utils();
    // floating, single line (k_EFloatingGamepadTextInputModeModeSingleLine), no field rectangle
    if (g_api.floating && g_api.floating(utils, 0, 0, 0, 0, 0)) {
        logx::Line(L"[steam] floating keyboard shown");
        return;
    }
    // full screen: normal input, single line
    if (g_api.full && g_api.full(utils, 0, 0, Utf8(description).c_str(), 64, Utf8(existing).c_str())) {
        logx::Line(L"[steam] full-screen keyboard shown");
        return;
    }
    logx::Line(L"[steam] keyboard: Steam refused both keyboards (the Steam overlay / Big Picture is needed)");
}

}  // namespace steam
