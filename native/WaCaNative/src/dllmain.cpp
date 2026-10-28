// WaCaNative: the native half of WaCa Core, a UE4SS C++ mod (Mods/WaCaCore/dlls/main.dll) built against the
// UE4SS SDK. Two kinds of work:
//  - the event bus (bridge.cpp): UE4SS's own ProcessEvent callbacks turned into events for every Lua mod, and the
//    WaCaNative_* functions Lua calls - built on UE4SS's hooks, nothing of ours patched in
//  - game-specific detours UE4SS has no hook for (input.cpp: the default key bindings; text.cpp / ui.cpp: string
//    and key tables), found by signature at load with MinHook
#include <Mod/CppUserModBase.hpp>

#include <string>

#include <windows.h>

#include "MinHook.h"
#include "appearance.h"
#include "camera.h"
#include "saves.h"
#include "bridge.h"
#include "input.h"
#include "keys.h"
#include "log.h"
#include "render.h"
#include "text.h"
#include "ui.h"

using namespace RC;

namespace {

// <...>\Mods\WaCaCore (the DLL sits in its dlls\ folder)
std::wstring ModDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModDir), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    p = p.substr(0, p.find_last_of(L"\\/"));          // ...\dlls
    return p.substr(0, p.find_last_of(L"\\/"));       // ...\WaCaCore
}

class WaCaNative : public CppUserModBase {
public:
    WaCaNative() {
        ModName = STR("WaCaCore");
        ModVersion = STR("1.0.0");
        ModDescription = STR("WaCa Core: the shared native API (event bus, game hooks) for WaCa mods");
        ModAuthors = STR("WaCa ModKit");
        auto dir = ModDir();
        logx::Open(dir + L"\\WaCaNative.log");
        logx::Line(L"WaCaNative loaded from %s", dir.c_str());
        // the detours go in at load (before the engine builds its default bindings)
        if (MH_Initialize() != MH_OK) {
            logx::Line(L"MinHook failed to initialise");
            return;
        }
        render::Install();
        keys::Install();
        input::Install(dir);
        text::Install(input::FNameCtor());
        ui::Init(input::FNameCtor());
        appearance::Install();
        camera::Install();
    }

    auto on_unreal_init() -> void override {
        bridge::Install();
        saves::Install();
    }

    auto on_lua_start(StringViewType modName, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                      LuaMadeSimple::Lua*) -> void override {
        bridge::LuaStarted(std::wstring(modName), lua);
    }

    auto on_lua_stop(StringViewType, LuaMadeSimple::Lua& lua, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&,
                     LuaMadeSimple::Lua*) -> void override {
        bridge::LuaStopping(lua);
    }
};

}  // namespace

extern "C" {
__declspec(dllexport) CppUserModBase* start_mod() { return new WaCaNative(); }
__declspec(dllexport) void uninstall_mod(CppUserModBase* mod) { delete mod; }
}
