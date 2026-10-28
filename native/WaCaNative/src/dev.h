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
void Stack(const std::wstring& label);
}
