#pragma once
#include <string>

namespace ui {
// fnameCtor = FName::FName(const TCHAR*, EFindName)
void Init(void* fnameCtor);
// rewrite one FUIButtonsMap row (key + hint icons) of the DataTable at tableHex
void SetUIKey(const std::wstring& tableHex, const std::wstring& row, const std::wstring& key, const std::wstring& icon);
// a row field's address (row data + offsetHex) as hex, empty when the row isn't there (dev::Watch)
std::wstring RowAddressHex(const std::wstring& tableHex, const std::wstring& row, const std::wstring& offsetHex);
}
