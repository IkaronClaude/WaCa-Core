// Key bindings: one detour on the game's "apply default bindings" step.
//
// The game (SwtPlayerInput) builds its default bindings natively - arrays of game::CommandMapping, each an ordinary
// engine action / axis mapping named after an ECommands value ("Player_Pause" = P, ...) - and applies them with one
// function that calls UPlayerInput::AddActionMapping / AddAxisMapping per entry. It runs again on every keyboard <->
// gamepad switch. The detour drops the keyboard / mouse defaults of every command the user rebinds, lets the game
// apply the rest, then registers the user's keys through the same engine functions. Gamepad bindings are untouched.
//
// keybinds.ini (next to the mod): [Actions] Command = Key[, Key ...]   [Axes] Axis = Key:Scale[, Key:Scale ...]
// ("Default" in a key list = the game's own keys for that command: Player_Interact = Default, LeftMouseButton)
//
// The key ICONS (the pause menu's controls page, the controls screen) come from the same default lists, built fresh by
// two SwtPlayerInput virtuals each time a key button asks (USwtPlayerInput +0x348 / +0x350, read from their call in
// the icon lookup); those are hooked too, so each command's first keyboard entry shows the key keybinds.ini gives it.
#include "input.h"

#include <windows.h>

#include <cstring>
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
using ResetKeyMapFn = void(__fastcall*)(void* keyMap);
using GetListFn = ue::TArray<game::CommandMapping>*(__fastcall*)(void* playerInput, ue::TArray<game::CommandMapping>* out);

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
// the key icon lookup (UGuiConfigButtonControl, 2026-11-01 build: 0x14127DD4A): "cmp sil, 1; jne; lea rdx, [rsp+0x20];
// call [rax + <list A>]; mov rbp, [rax]; mov [rax], r15" ... "call [rax + <list B>]" at +0x3F
constexpr const char* kIconListsSig = "40 80 FE 01 75 ?? 48 8D 54 24 20 FF 90 ?? ?? ?? ?? 48 8B 28 4C 89 38";
constexpr size_t kIconListA = 0x0B, kIconListB = 0x3F;

ApplyFn g_applyOrig = nullptr;
AddActionFn g_addAction = nullptr;
AddAxisFn g_addAxis = nullptr;
FNameCtorFn g_fnameCtor = nullptr;
FNameToStringFn g_fnameToString = nullptr;

// UPlayerInput's own layout, read from AddActionMapping / AddAxisMapping (2026-11-01 build: "add rcx, 0x1C0" -
// ActionMappings, "lea rcx, [rbx+0x240]; call" - ActionKeyMap.Reset, "and byte ptr [rbx+0x334], 0xFE" - bKeyMapsBuilt;
// AddAxisMapping: 0x1D0 / 0x290). Needed to change the bindings while playing (input::ApplyNow).
struct Layout {
    uint32_t mappings = 0, keyMap = 0, builtFlag = 0;
    ResetKeyMapFn reset = nullptr;
};
Layout g_actionLayout, g_axisLayout;
void* g_playerInput = nullptr;    // the player's input, as the game last applied its bindings to it
// the game's own keyboard / mouse defaults, as its last keyboard list had them (to give back a command the user
// unbinds: ApplyNow)
std::map<std::wstring, std::vector<std::wstring>> g_defaultActions;
std::map<std::wstring, std::vector<std::pair<std::wstring, float>>> g_defaultAxes;
// the default-list getters (the icons' source): their vtable slots, and the originals once hooked
uint32_t g_listSlots[2] = {0, 0};
GetListFn g_listOrig[2] = {nullptr, nullptr};
bool g_listsHooked = false;
bool g_defaultsFromLists = false;   // the defaults were read from a getter's list before it was rewritten

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
// THE PLAYER'S OWN KEYS: keybinds.user.ini (no release ships it, so an update never replaces it) is read after
// keybinds.ini - a line there wins, "Command = -" removes the shipped one (the game's own keys then). The controls
// screen writes there. Same rule as WaCa Core's Lua ReadIni.
std::filesystem::file_time_type g_userTime{};
std::wstring UserPath() {
    auto dot = g_configPath.rfind(L".ini");
    return dot == std::wstring::npos ? std::wstring() : g_configPath.substr(0, dot) + L".user.ini";
}
static void ReadBindings(const std::wstring& path) {
    std::wifstream f(path);
    if (!f) return;
    std::wstring line, section;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') { section = line.substr(1, line.size() - 2); continue; }
        auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        auto name = Trim(line.substr(0, eq));
        auto value = Trim(line.substr(eq + 1));
        if (section == L"Actions") {
            if (value == L"-") { g_actions.erase(name); continue; }
            g_actions[name] = Split(value, L',');
        } else if (section == L"Axes") {
            if (value == L"-") { g_axes.erase(name); continue; }
            auto& list = g_axes[name];
            list.clear();
            for (auto& k : Split(value, L',')) {
                auto colon = k.find(L':');
                float scale = colon == std::wstring::npos ? 1.0f : std::stof(k.substr(colon + 1));
                list.emplace_back(Trim(k.substr(0, colon)), scale);
            }
        }
    }
}
void LoadConfig(bool force = false) {
    std::error_code ec, ecUser;
    auto t = std::filesystem::last_write_time(g_configPath, ec);
    auto u = std::filesystem::last_write_time(UserPath(), ecUser);
    if (ecUser) u = std::filesystem::file_time_type{};
    if (ec || (!force && t == g_configTime && u == g_userTime)) return;
    g_configTime = t;
    g_userTime = u;
    g_actions.clear();
    g_axes.clear();
    ReadBindings(g_configPath);
    size_t shipped = g_actions.size() + g_axes.size();
    ReadBindings(UserPath());
    logx::Line(L"[input] keybinds.ini loaded: %zu action and %zu axis overrides (%s)", g_actions.size(), g_axes.size(),
               ecUser ? L"no keybinds.user.ini" : L"keybinds.user.ini laid over it");
    (void)shipped;
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

// a command's configured keys with "Default" replaced by the game's own keys for it
std::vector<std::wstring> ActionKeys(const std::wstring& name, const std::vector<std::wstring>& keys) {
    std::vector<std::wstring> out;
    for (auto& k : keys) {
        if (_wcsicmp(k.c_str(), L"Default") != 0) { out.push_back(k); continue; }
        auto def = g_defaultActions.find(name);
        if (def != g_defaultActions.end()) out.insert(out.end(), def->second.begin(), def->second.end());
    }
    return out;
}
std::vector<std::pair<std::wstring, float>> AxisKeys(const std::wstring& name, const std::vector<std::pair<std::wstring, float>>& keys) {
    std::vector<std::pair<std::wstring, float>> out;
    for (auto& k : keys) {
        if (_wcsicmp(k.first.c_str(), L"Default") != 0) { out.push_back(k); continue; }
        auto def = g_defaultAxes.find(name);
        if (def != g_defaultAxes.end()) out.insert(out.end(), def->second.begin(), def->second.end());
    }
    return out;
}

// a key of a default list set to another key (its cached key details let go: the engine looks them up again)
void SetKey(ue::FKey& key, const std::wstring& name) {
    key.KeyName = MakeName(name);
    key.KeyDetails = nullptr;       // (the old details' reference is left as is - they are static)
    key.KeyDetailsRef = nullptr;
}

// a default list as the icons read it: each configured command's first keyboard entry (per direction, for an axis)
// gets the first key keybinds.ini gives it - the icon shows that key
void RewriteForIcons(ue::TArray<game::CommandMapping>* list) {
    if (!list || !list->Data) return;
    LoadConfig();
    // (the game's own keys first, as built - the defaults "Default" and an unbound command go back to)
    std::map<std::wstring, std::vector<std::wstring>> defaultActions;
    std::map<std::wstring, std::vector<std::pair<std::wstring, float>>> defaultAxes;
    for (int32_t i = 0; i < list->Num; ++i) {
        auto& m = list->Data[i];
        if (m.Kind != game::KindAction && m.Kind != game::KindAxis) continue;
        bool action = m.Kind == game::KindAction;
        std::wstring keyName = NameOf(action ? m.Action.Key.KeyName : m.Axis.Key.KeyName);
        if (IsGamepadKey(keyName)) continue;
        std::wstring name = NameOf(action ? m.Action.ActionName : m.Axis.AxisName);
        if (action) defaultActions[name].push_back(keyName);
        else defaultAxes[name].emplace_back(keyName, m.Axis.Scale);
    }
    if (!defaultActions.empty() || !defaultAxes.empty()) {
        g_defaultActions = std::move(defaultActions);
        g_defaultAxes = std::move(defaultAxes);
        g_defaultsFromLists = true;
    }
    std::set<std::wstring> doneActions, doneAxes;
    for (int32_t i = 0; i < list->Num; ++i) {
        auto& m = list->Data[i];
        bool action = m.Kind == game::KindAction;
        if (!action && m.Kind != game::KindAxis) continue;
        auto& key = action ? m.Action.Key : m.Axis.Key;
        if (IsGamepadKey(NameOf(key.KeyName))) continue;
        std::wstring name = NameOf(action ? m.Action.ActionName : m.Axis.AxisName);
        if (action) {
            auto it = g_actions.find(name);
            if (it == g_actions.end() || !doneActions.insert(name).second) continue;
            auto keys = ActionKeys(name, it->second);
            if (!keys.empty()) SetKey(key, keys.front());
        } else {
            auto it = g_axes.find(name);
            bool positive = m.Axis.Scale > 0;
            if (it == g_axes.end() || !doneAxes.insert(name + (positive ? L"+" : L"-")).second) continue;
            for (auto& [k, scale] : AxisKeys(name, it->second)) {
                if ((scale > 0) == positive) { SetKey(key, k); break; }
            }
        }
    }
}

template <int N>
ue::TArray<game::CommandMapping>* __fastcall ListDetour(void* playerInput, ue::TArray<game::CommandMapping>* out) {
    auto* list = g_listOrig[N](playerInput, out);
    RewriteForIcons(list);
    return list;
}

// the getters hooked from the player input's own vtable, the first time the game applies its bindings
void HookLists(void* playerInput) {
    if (g_listsHooked || !g_listSlots[0] || !g_listSlots[1]) return;
    g_listsHooked = true;
    auto** vtable = *reinterpret_cast<void***>(playerInput);
    void* detours[2] = {reinterpret_cast<void*>(&ListDetour<0>), reinterpret_cast<void*>(&ListDetour<1>)};
    for (int n = 0; n < 2; ++n) {
        void* target = vtable[g_listSlots[n] / sizeof(void*)];
        if (n == 1 && target == vtable[g_listSlots[0] / sizeof(void*)]) {
            g_listOrig[1] = g_listOrig[0];      // (one function in both slots: hooked once)
            break;
        }
        if (MH_CreateHook(target, detours[n], reinterpret_cast<void**>(&g_listOrig[n])) != MH_OK || MH_EnableHook(target) != MH_OK) {
            logx::Line(L"[input] could not hook the default list getter +0x%X - key icons show the game's keys", g_listSlots[n]);
            continue;
        }
    }
    logx::Line(L"[input] key icons follow keybinds.ini (default list getters +0x%X / +0x%X hooked)", g_listSlots[0], g_listSlots[1]);
}

void __fastcall ApplyDetour(void* playerInput, ue::TArray<game::CommandMapping>* mappings) {
    LoadConfig();
    g_playerInput = playerInput;
    HookLists(playerInput);
    bool keyboardList = false;
    bool logThis = g_loggedLists.insert(mappings->Num).second;
    std::map<std::wstring, std::vector<std::wstring>> defaultActions;
    std::map<std::wstring, std::vector<std::pair<std::wstring, float>>> defaultAxes;
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
        if (action) defaultActions[name].push_back(keyName);
        else defaultAxes[name].emplace_back(keyName, m.Axis.Scale);
        if ((action && g_actions.count(name)) || (!action && g_axes.count(name))) {
            m.Kind = game::KindDropped;
        }
    }
    g_applyOrig(playerInput, mappings);
    if (!keyboardList) return;
    if (!g_defaultsFromLists) {     // (once the getters are hooked they read the defaults before their rewrite)
        g_defaultActions = std::move(defaultActions);
        g_defaultAxes = std::move(defaultAxes);
    }
    for (auto& [command, configured] : g_actions) {
        for (auto& k : ActionKeys(command, configured)) {
            ue::FInputActionKeyMapping m;
            m.ActionName = MakeName(command);
            m.Key.KeyName = MakeName(k);
            g_addAction(playerInput, &m);
        }
    }
    for (auto& [axis, configured] : g_axes) {
        for (auto& [k, scale] : AxisKeys(axis, configured)) {
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

// AddActionMapping / AddAxisMapping's code: add rcx, <mappings>; call; lea rcx, [rbx + <key map>]; call <reset>;
// and byte ptr [rbx + <flag>], 0xFE
bool ReadLayout(const uint8_t* f, Layout& out) {
    if (!(f[0x09] == 0x48 && f[0x0A] == 0x81 && f[0x0B] == 0xC1 && f[0x10] == 0xE8 && f[0x15] == 0x48 &&
          f[0x16] == 0x8D && f[0x17] == 0x8B && f[0x1C] == 0xE8 && f[0x21] == 0x80 && f[0x22] == 0xA3 && f[0x27] == 0xFE)) {
        return false;
    }
    out.mappings = *reinterpret_cast<const uint32_t*>(f + 0x0C);
    out.keyMap = *reinterpret_cast<const uint32_t*>(f + 0x18);
    out.builtFlag = *reinterpret_cast<const uint32_t*>(f + 0x23);
    out.reset = reinterpret_cast<ResetKeyMapFn>(scan::RelTarget(const_cast<uint8_t*>(f + 0x1C)));
    return true;
}

// drops the keyboard / mouse mappings of the named commands from a live mapping array (gamepad ones stay)
template <typename Mapping, typename NameOfMapping>
int DropKeyboard(ue::TArray<Mapping>* list, const std::set<std::wstring>& names, NameOfMapping nameOf) {
    int kept = 0, dropped = 0;
    for (int32_t i = 0; i < list->Num; ++i) {
        Mapping& m = list->Data[i];
        if (names.count(NameOf(nameOf(m))) && !IsGamepadKey(NameOf(m.Key.KeyName))) {
            ++dropped;      // (its key details' shared reference is left as is - a few bytes, the details are static)
            continue;
        }
        if (kept != i) std::memcpy(&list->Data[kept], &m, sizeof(Mapping));
        ++kept;
    }
    list->Num = kept;
    return dropped;
}

}  // namespace

namespace input {

// a command's keyboard / mouse keys as they are now: keybinds.ini's ("Default" expanded), else the game's own
std::vector<std::wstring> BoundKeys(const std::wstring& command) {
    LoadConfig();
    if (auto it = g_actions.find(command); it != g_actions.end()) return ActionKeys(command, it->second);
    if (auto def = g_defaultActions.find(command); def != g_defaultActions.end()) return def->second;
    std::vector<std::wstring> out;
    auto axis = g_axes.find(command);
    for (auto& [k, scale] : axis != g_axes.end() ? AxisKeys(command, axis->second)
                                                 : (g_defaultAxes.count(command) ? g_defaultAxes[command]
                                                                                 : std::vector<std::pair<std::wstring, float>>{})) {
        out.push_back(k);
    }
    return out;
}

// an axis's keyboard / mouse keys with their scales as they are now: keybinds.ini's ("Default" expanded), else the
// game's own (the controls screen's direction rows, 2026-11-02)
std::vector<std::pair<std::wstring, float>> BoundAxisKeys(const std::wstring& axis) {
    LoadConfig();
    if (auto it = g_axes.find(axis); it != g_axes.end()) return AxisKeys(axis, it->second);
    if (auto def = g_defaultAxes.find(axis); def != g_defaultAxes.end()) return def->second;
    return {};
}

// keybinds.ini again, now: every command it names - or named before - gets its keys from it, or back from the
// game's defaults when the file no longer names it. On the game thread (the engine's input is not locked).
int ApplyNow() {
    if (!g_playerInput || !g_actionLayout.reset || !g_axisLayout.reset) return -1;
    std::set<std::wstring> actions, axes;
    for (auto& [n, _] : g_actions) actions.insert(n);
    for (auto& [n, _] : g_axes) axes.insert(n);
    LoadConfig(true);
    for (auto& [n, _] : g_actions) actions.insert(n);
    for (auto& [n, _] : g_axes) axes.insert(n);
    auto* base = static_cast<uint8_t*>(g_playerInput);
    auto* actionList = reinterpret_cast<ue::TArray<ue::FInputActionKeyMapping>*>(base + g_actionLayout.mappings);
    auto* axisList = reinterpret_cast<ue::TArray<ue::FInputAxisKeyMapping>*>(base + g_axisLayout.mappings);
    int dropped = DropKeyboard(actionList, actions, [](const ue::FInputActionKeyMapping& m) { return m.ActionName; }) +
                  DropKeyboard(axisList, axes, [](const ue::FInputAxisKeyMapping& m) { return m.AxisName; });
    int added = 0;
    for (auto& name : actions) {
        auto it = g_actions.find(name);
        std::vector<std::wstring> keys;
        if (it != g_actions.end()) keys = ActionKeys(name, it->second);
        else if (auto def = g_defaultActions.find(name); def != g_defaultActions.end()) keys = def->second;
        for (auto& k : keys) {
            ue::FInputActionKeyMapping m;
            m.ActionName = MakeName(name);
            m.Key.KeyName = MakeName(k);
            g_addAction(g_playerInput, &m);
            ++added;
        }
    }
    for (auto& name : axes) {
        auto it = g_axes.find(name);
        std::vector<std::pair<std::wstring, float>> keys;
        if (it != g_axes.end()) keys = AxisKeys(name, it->second);
        else if (auto def = g_defaultAxes.find(name); def != g_defaultAxes.end()) keys = def->second;
        for (auto& [k, scale] : keys) {
            ue::FInputAxisKeyMapping m;
            m.AxisName = MakeName(name);
            m.Scale = scale;
            m.Key.KeyName = MakeName(k);
            g_addAxis(g_playerInput, &m);
            ++added;
        }
    }
    // (the engine rebuilds its key lookup only from an empty one - what AddActionMapping does after each add)
    g_actionLayout.reset(base + g_actionLayout.keyMap);
    g_axisLayout.reset(base + g_axisLayout.keyMap);
    base[g_actionLayout.builtFlag] &= 0xFE;
    logx::Line(L"[input] bindings applied now: %zu actions / %zu axes from keybinds.ini (%d keys dropped, %d added)",
               g_actions.size(), g_axes.size(), dropped, added);
    return added;
}

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
    if (auto icons = scan::Find(kIconListsSig); icons && (*icons)[kIconListB] == 0xFF && (*icons)[kIconListB + 1] == 0x90) {
        g_listSlots[0] = *reinterpret_cast<const uint32_t*>(*icons + kIconListA + 2);
        g_listSlots[1] = *reinterpret_cast<const uint32_t*>(*icons + kIconListB + 2);
    } else {
        logx::Line(L"[input] the key icon lookup was not found - key icons show the game's keys (game update?)");
    }
    if (!ReadLayout(reinterpret_cast<const uint8_t*>(g_addAction), g_actionLayout) ||
        !ReadLayout(reinterpret_cast<const uint8_t*>(g_addAxis), g_axisLayout)) {
        logx::Line(L"[input] AddActionMapping's layout changed - bindings apply only when the game rebuilds them");
    } else {
        logx::Line(L"[input] player input: actions +0x%X (map +0x%X), axes +0x%X (map +0x%X), built flag +0x%X",
                   g_actionLayout.mappings, g_actionLayout.keyMap, g_axisLayout.mappings, g_axisLayout.keyMap,
                   g_actionLayout.builtFlag);
    }
    if (MH_CreateHook(a, reinterpret_cast<void*>(&ApplyDetour), reinterpret_cast<void**>(&g_applyOrig)) != MH_OK ||
        MH_EnableHook(a) != MH_OK) {
        logx::Line(L"[input] could not hook the apply function");
        return false;
    }
    logx::Line(L"[input] key rebinding active (AddActionMapping %p, AddAxisMapping %p)", g_addAction, g_addAxis);
    return true;
}

}  // namespace input
