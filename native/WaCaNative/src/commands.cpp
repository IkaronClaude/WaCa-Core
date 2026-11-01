// The commands Lua sends with WaCaNative.Command (registered by bridge.cpp in every Lua mod's state):
//   SetText|<key>|<text>
//   SetUIKey|<DataTable address hex>|<row>|<key>[|<icon texture object path>]   (ui.cpp)
//   SteamKeyboard|<description>|<existing text>      the answer arrives as the SteamTextInput event (steam.cpp)
//   DevWatch|<address hex>|<length>  DevWatchRow|<table hex>|<row>|<offset hex>|<length>  DevUnwatch
//   PressKey|<Unreal key name>                      a real key press + release posted to the game window (KeyAliases)
//   HoldKey|<Unreal key name>|<1/0>                 the key held down / let go (a real key message) (dev.cpp)
//   SwallowKey|<Unreal key name>|<1/0>              the game doesn't get that key (still KeyDown / KeyUp) (keys.cpp)
//   SwallowAll|<1/0>                                the game gets no key / mouse button (a mod's own menu on top)
//   ApplyBindings                                   keybinds.ini applied to the live bindings now (input.cpp)
//   Party|<role>,<role>                             the next fights' party = those narrative roles; Party| = the game's (party.cpp)
//   DevPressKey|<vk hex>  DevStack|<label>                                        (dev.cpp)
//   MouseLook|<0/1>|<yaw degrees per mouse unit>|<pitch degrees per mouse unit>   mouse look on / off (camera.cpp)
//   CameraRelativeMove|<0/1>                         right / left strafe instead of turning the camera (camera.cpp)
//   PreyFlee|<0/1>|<size tags that keep the game's reaction>   prey run instead of reacting (prey.cpp)
//   PreyPeace|<CharacterType values, comma separated>      those prey start no fight when touched (prey.cpp)
//   HuntLoud|...                 a no-op (kept for old Lua)
//   HuntFail|<prey full name>    the pounce under way on that prey fails (hunt.cpp)
//   HuntRoll|<0/1>|<base chance %>|<critical miss %>|<CharacterType value>=<factor>,...   (hunt.cpp)
//   CombatScale|<dealt x>|<taken x>   the player's cat's combat damage (combat.cpp)
#include "commands.h"

#include "camera.h"
#include "combat.h"
#include "prey.h"
#include "hunt.h"
#include "dev.h"
#include "input.h"
#include "party.h"
#include "keys.h"
#include "log.h"
#include "steam.h"
#include "text.h"
#include "ui.h"

namespace {

std::wstring Arg(const std::wstring& s, size_t i) {
    size_t start = 0;
    for (size_t k = 0; k < i; ++k) {
        start = s.find(L'|', start);
        if (start == std::wstring::npos) return L"";
        ++start;
    }
    size_t end = s.find(L'|', start);
    return s.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

}  // namespace

namespace commands {

bool Run(const std::wstring& cmd) {
    auto verb = Arg(cmd, 0);
    if (verb == L"SetText") text::SetText(Arg(cmd, 1), Arg(cmd, 2));
    else if (verb == L"SetUIKey") ui::SetUIKey(Arg(cmd, 1), Arg(cmd, 2), Arg(cmd, 3), Arg(cmd, 4));
    else if (verb == L"DevWatch") dev::Watch(Arg(cmd, 1), Arg(cmd, 2));
    else if (verb == L"DevWatchRow") dev::Watch(ui::RowAddressHex(Arg(cmd, 1), Arg(cmd, 2), Arg(cmd, 3)), Arg(cmd, 4));
    else if (verb == L"DevUnwatch") dev::Unwatch();
    else if (verb == L"DevPressKey") dev::PressKey(Arg(cmd, 1));
    else if (verb == L"PressKey") dev::PressKeyNamed(Arg(cmd, 1));
    else if (verb == L"HoldKey") dev::HoldKeyNamed(Arg(cmd, 1), Arg(cmd, 2) == L"1");
    else if (verb == L"SwallowKey") keys::Swallow(dev::VirtualKey(Arg(cmd, 1)), Arg(cmd, 2) == L"1");
    else if (verb == L"SwallowAll") keys::SwallowAll(Arg(cmd, 1) == L"1");
    else if (verb == L"ApplyBindings") input::ApplyNow();
    else if (verb == L"Party") return party::Set(Arg(cmd, 1));
    else if (verb == L"DevStack") dev::Stack(Arg(cmd, 1));
    else if (verb == L"SteamKeyboard") steam::ShowKeyboard(Arg(cmd, 1), Arg(cmd, 2));
    else if (verb == L"HuntRoll") hunt::Configure(cmd);
    else if (verb == L"CombatScale") combat::Configure(cmd);
    else if (verb == L"HuntLoud") hunt::SetLoud(Arg(cmd, 1) == L"1", std::wcstod(Arg(cmd, 2).c_str(), nullptr));
    else if (verb == L"HuntFail") hunt::Fail(Arg(cmd, 1));
    else if (verb == L"PreyFlee") prey::Configure(Arg(cmd, 1) == L"1", Arg(cmd, 2));
    else if (verb == L"PreyPeace") prey::Peace(Arg(cmd, 1));
    else if (verb == L"CameraRelativeMove") camera::SetCameraRelativeMove(Arg(cmd, 1) == L"1");
    else if (verb == L"MouseLook") camera::SetMouseLook(Arg(cmd, 1) == L"1", std::wcstof(Arg(cmd, 2).c_str(), nullptr),
                                                         std::wcstof(Arg(cmd, 3).c_str(), nullptr));
    else {
        logx::Line(L"[commands] unknown command %s", cmd.c_str());
        return false;
    }
    return true;
}

}  // namespace commands
