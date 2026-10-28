#pragma once
#include <string>

// Steam's on-screen keyboard (see steam.cpp)
namespace steam {
void ShowKeyboard(const std::wstring& description, const std::wstring& existing);
}
