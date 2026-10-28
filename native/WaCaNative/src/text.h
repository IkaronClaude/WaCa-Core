#pragma once
#include <string>

namespace text {
// String-table text overrides. fnameCtor = FName::FName(const TCHAR*, EFindName).
bool Install(void* fnameCtor);
// rewrite one entry of the game's string table (SwtStrings)
void SetText(const std::wstring& key, const std::wstring& text);
}
