#pragma once
#include <string>

// Reverse-engineering helpers (see dev.cpp): a hardware watchpoint that logs the code touching an address, and key
// presses posted to the game window.
namespace dev {
void Watch(const std::wstring& addrHex, const std::wstring& lenText);
void Unwatch();
void PressKey(const std::wstring& vkHex);
// a real key press + release, by its Unreal key name ("BackSpace"): KeyAliases use it - menus only see real input
void PressKeyNamed(const std::wstring& keyName);
// a key held down / let go (a real key message each), by its Unreal key name - the map is panned with held keys
void HoldKeyNamed(const std::wstring& keyName, bool down);
// an Unreal key name ("BackSpace", "Enter", "A", "F5", "One") as a Windows virtual key, 0 when unknown
unsigned int VirtualKey(const std::wstring& name);
void Stack(const std::wstring& label);
}
