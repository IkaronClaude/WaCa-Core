#pragma once
#include <string>

// Prey that flee instead of reacting to the player (see prey.cpp)
namespace prey {
// hook the prey's detection reaction from a live prey (its vtable); once, on the first prey Lua reports
bool HookFrom(void* preyObject);
// PreyFlee|<0/1>|<comma separated size tags that keep the game's reaction>
void Configure(bool on, const std::wstring& excludedTags);
}
