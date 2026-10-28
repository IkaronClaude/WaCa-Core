#pragma once
#include <string>

// The player's combat strength by specialisation (see combat.cpp)
namespace combat {
// register the ProcessEvent callback (once, with the bridge's)
void Install();
// CombatScale|<dealt x>|<taken x>: damage the player's own cat deals / takes is multiplied (1|1 = the game's)
void Configure(const std::wstring& command);
}
