#pragma once
#include <string>
#include <vector>

namespace input {
// Hooks the game's default-binding apply step; keybinds.ini lives in <modDir>\config. False when the game changed.
bool Install(const std::wstring& modDir);
// FName::FName(const TCHAR*, EFindName), once Install found it (else null)
void* FNameCtor();
// keybinds.ini applied to the player's live bindings now (a command it no longer names gets the game's keys back);
// the keys added, or -1 when the game hasn't built its bindings yet / the layout is unknown. Game thread.
int ApplyNow();
// a command's (or axis's) keyboard / mouse keys as they are now - keybinds.ini's, else the game's own; empty = unknown
std::vector<std::wstring> BoundKeys(const std::wstring& command);
}
