// Key bindings: one detour on the game's "apply default bindings" step.
//
// The game (SwtPlayerInput) builds its default bindings natively - arrays of game::CommandMapping, each an ordinary
// engine action / axis mapping named after an ECommands value ("Player_Pause" = P, ...) - and applies them with one
// function that calls UPlayerInput::AddActionMapping / AddAxisMapping per entry. It runs again on every keyboard <->
// gamepad switch. The detour drops the keyboard / mouse defaults of every command the user rebinds, lets the game
// apply the rest, then registers the user's keys through the same engine functions. Gamepad bindings are untouched.
//
// keybinds.ini (next to the mod): [Actions] Command = Key[, Key ...]   [Axes] Axis = Key:Scale[, Key:Scale ...]
#include "input.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "scan.h"
#include "ue.h"

namespace {

using ApplyFn = void(__fastcall*)(void* playerInput, ue::TArray<game::CommandMapping>* mappings);
using AddActionFn = void(__fastcall*)(void* playerInput, const ue::FInputActionKeyMapping* mapping);
using AddAxisFn = void(__fastcall*)(void* playerInput, const ue::FInputAxisKeyMapping* mapping);
using FNameCtorFn = void(__fastcall*)(ue::FName* out, const wchar_t* name, int findType);
using FNameToStringFn = void(__fastcall*)(const ue::FName* name, ue::FString* out);

// SwtPlayerInput's apply step (2026-10-24 build: 0x14128B150). It ends at the call to UPlayerInput::AddActionMapping;
// the AddAxisMapping call follows at +0x68.
constexpr const char* kApplySig =
    "48 83 EC 28 48 89 5C 24 30 33 DB 48 89 6C 24 38 48 8B E9 48 89 74 24 40 48 8B F2 8B 52 08 85 D2 7E 5D 48 89 7C 24 "
    "20 8B FB 0F 1F 80 00 00 00 00 48 8B 16 0F B6 04 17 84 C0 75 18 48 63 C3 48 83 C2 08 48 6B C8 58 48 03 D1 48 8B CD "
    "E8 ?? ?? ?? ??";
constexpr size_t kApplyAddActionCall = 76;
constexpr size_t kApplyAddAxisCall = 0x68;
// FName::FName(const TCHAR*, EFindName) and FName::ToString(FString&) (the same functions UE4SS finds)
constexpr const char* kFNameCtorSig = "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54 24 20 33 C9";
constexpr const char* kFNameToStringSig = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 83 79 04 00 48";

ApplyFn g_applyOrig = nullptr;
AddActionFn g_addAction = nullptr;
AddAxisFn g_addAxis = nullptr;
FNameCtorFn g_fnameCtor = nullptr;
FNameToStringFn g_fnameToString = nullptr;

std::wstring g_configPath;
std::filesystem::file_time_type g_configTime{};
std::map<std::wstring, std::vector<std::wstring>> g_actions;                    // command -> keys
std::map<std::wstring, std::vector<std::pair<std::wstring, float>>> g_axes;     // axis -> (key, scale)
std::set<int32_t> g_loggedLists;                  // the default lists already written to the log (by size)

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, sep)) {
        item = Trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

// (re)reads keybinds.ini when it changed: a binding edit applies on the game's next rebuild of its bindings
void LoadConfig() {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(g_configPath, ec);
    if (ec || t == g_configTime) return;
    g_configTime = t;
    g_actions.clear();
    g_axes.clear();
    std::wifstream f(g_configPath);
    std::wstring line, section;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') { section = line.substr(1, line.size() - 2); continue; }
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        auto name = Trim(line.substr(0, eq));
        auto keys = Split(line.substr(eq + 1), L',');
        if (section == L"Actions") {
            g_actions[name] = keys;
        } else if (section == L"Axes") {
            auto& list = g_axes[name];
            for (auto& k : keys) {
                auto colon = k.find(L':');
                float scale = colon == std::wstring::npos ? 1.0f : std::stof(k.substr(colon + 1));
                list.emplace_back(Trim(k.substr(0, colon)), scale);
            }
        }
    }
    logx::Line(L"[input] keybinds.ini loaded: %zu action and %zu axis overrides", g_actions.size(), g_axes.size());
}

std::wstring NameOf(const ue::FName& n) {
    ue::FString s;
    g_fnameToString(&n, &s);
    return s.c_str();   // the engine's buffer is left to the engine allocator (a few bytes per rebuild, log only)
}

ue::FName MakeName(const std::wstring& s) {
    ue::FName n;
    g_fnameCtor(&n, s.c_str(), 1 /* FNAME_Add */);
    return n;
}

bool IsGamepadKey(const std::wstring& key) {
    return key.rfind(L"Gamepad_", 0) == 0 || key.rfind(L"Steam", 0) == 0 || key.rfind(L"MotionController", 0) == 0;
}

void __fastcall ApplyDetour(void* playerInput, ue::TArray<game::CommandMapping>* mappings) {
    LoadConfig();
    bool keyboardList = false;
    bool logThis = g_loggedLists.insert(mappings->Num).second;
    for (int32_t i = 0; i < mappings->Num; ++i) {
        auto& m = mappings->Data[i];
        bool action = m.Kind == game::KindAction;
        const auto& key = action ? m.Action.Key : m.Axis.Key;
        std::wstring name = NameOf(action ? m.Action.ActionName : m.Axis.AxisName);
        std::wstring keyName = NameOf(key.KeyName);
        if (logThis) {
            logx::Line(L"[input] default %s #%u %s = %s%s", action ? L"action" : L"axis", m.Command, name.c_str(),
                       keyName.c_str(), action ? L"" : (L" x" + std::to_wstring(m.Axis.Scale)).c_str());
        }
        if (IsGamepadKey(keyName)) continue;
        keyboardList = true;
        if ((action && g_actions.count(name)) || (!action && g_axes.count(name))) {
            m.Kind = game::KindDropped;
        }
    }
    g_applyOrig(playerInput, mappings);
    if (!keyboardList) return;
    for (auto& [command, keys] : g_actions) {
        for (auto& k : keys) {
            ue::FInputActionKeyMapping m;
            m.ActionName = MakeName(command);
            m.Key.KeyName = MakeName(k);
            g_addAction(playerInput, &m);
        }
    }
    for (auto& [axis, keys] : g_axes) {
        for (auto& [k, scale] : keys) {
            ue::FInputAxisKeyMapping m;
            m.AxisName = MakeName(axis);
            m.Scale = scale;
            m.Key.KeyName = MakeName(k);
            g_addAxis(playerInput, &m);
        }
    }
    logx::Line(L"[input] bindings applied (%d defaults, %zu actions / %zu axes from keybinds.ini)", mappings->Num,
               g_actions.size(), g_axes.size());
}

}  // namespace

namespace input {

void* FNameCtor() { return reinterpret_cast<void*>(g_fnameCtor); }

// a UE4SS mod is on: its folder has enabled.txt, or mods.txt lists it as "Name : 1"
bool ModEnabled(const std::wstring& modsDir, const std::wstring& name) {
    std::error_code ec;
    if (!std::filesystem::is_directory(modsDir + L"\\" + name, ec)) return false;
    if (std::filesystem::exists(modsDir + L"\\" + name + L"\\enabled.txt", ec)) return true;
    std::wifstream f(modsDir + L"\\mods.txt");
    std::wstring line;
    while (std::getline(f, line)) {
        auto colon = line.find(L':');
        if (colon == std::wstring::npos || Trim(line.substr(0, colon)) != name) continue;
        return Trim(line.substr(colon + 1)) == L"1";
    }
    return false;
}

bool Install(const std::wstring& modDir) {
    // The key bindings belong to the Keybinds & Mouse mod (WaCaKeybinds\config\keybinds.ini), read here because the
    // game builds its bindings before any Lua runs. Without that mod (or with it off): the game's own bindings.
    std::wstring modsDir = modDir.substr(0, modDir.find_last_of(L"\\/"));
    if (ModEnabled(modsDir, L"WaCaKeybinds")) {
        g_configPath = modsDir + L"\\WaCaKeybinds\\config\\keybinds.ini";
    } else {
        g_configPath = modDir + L"\\config\\keybinds.ini";            // (none shipped: the game's own bindings)
        logx::Line(L"[input] the Keybinds & Mouse mod (WaCaKeybinds) is not installed / on - the game's own bindings");
    }
    auto apply = scan::Find(kApplySig);
    auto ctor = scan::Find(kFNameCtorSig);
    auto toString = scan::Find(kFNameToStringSig);
    logx::Line(L"[input] apply %p, FName ctor %p, FName::ToString %p", apply.value_or(nullptr), ctor.value_or(nullptr),
               toString.value_or(nullptr));
    if (!apply || !ctor || !toString) {
        logx::Line(L"[input] a signature was not found - key rebinding is OFF (game update?)");
        return false;
    }
    uint8_t* a = *apply;
    if (a[kApplyAddActionCall] != 0xE8 || a[kApplyAddAxisCall] != 0xE8) {
        logx::Line(L"[input] the apply function's layout changed - key rebinding is OFF");
        return false;
    }
    g_addAction = reinterpret_cast<AddActionFn>(scan::RelTarget(a + kApplyAddActionCall));
    g_addAxis = reinterpret_cast<AddAxisFn>(scan::RelTarget(a + kApplyAddAxisCall));
    g_fnameCtor = reinterpret_cast<FNameCtorFn>(*ctor);
    g_fnameToString = reinterpret_cast<FNameToStringFn>(*toString);
    if (MH_CreateHook(a, reinterpret_cast<void*>(&ApplyDetour), reinterpret_cast<void**>(&g_applyOrig)) != MH_OK ||
        MH_EnableHook(a) != MH_OK) {
        logx::Line(L"[input] could not hook the apply function");
        return false;
    }
    logx::Line(L"[input] key rebinding active (AddActionMapping %p, AddAxisMapping %p)", g_addAction, g_addAxis);
    return true;
}

}  // namespace input
