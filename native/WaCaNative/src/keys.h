#pragma once

// Raw key / mouse button presses as the Lua event KeyDown, mouse moves as MouseMove (see keys.cpp)
namespace keys {
void Install();
// the mouse in desktop pixels
void CursorPos(long& x, long& y);
// while on: the key still raises KeyDown / KeyUp, but the game never gets it (its key and character messages are
// dropped) - the name screen keeps Enter from the game while you are on the list
void Swallow(unsigned int vk, bool on);
// every key, mouse button and wheel kept from the game (still KeyDown / KeyUp / MouseWheel): a mod's own menu on top
void SwallowAll(bool on);
}
