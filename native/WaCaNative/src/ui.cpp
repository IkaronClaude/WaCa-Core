// UI button rows: the menus read their keyboard keys + hint icons from DataTables of FUIButtonsMap rows keyed by
// command name (UXD/Buttons/DT_GamepadButtonsKeyboard: Button_B / Button_Back = BackSpace, Button_A = Enter, ...).
// The rows aren't reachable through reflection (UDataTable::RowMap is native), so Lua sends the table's address and
// this rewrites a row in place: its key, and both icons pointing at the new key's icon texture.
#include "ui.h"

#include <windows.h>

#include <string>

#include "log.h"
#include "ue.h"

namespace {

using FNameCtorFn = void(__fastcall*)(ue::FName* out, const wchar_t* name, int findType);
FNameCtorFn g_fnameCtor = nullptr;

ue::FName Name(const std::wstring& s) {
    ue::FName n;
    g_fnameCtor(&n, s.c_str(), 1);
    return n;
}

// UE 5.7 layouts used here
constexpr size_t kRowMapOffset = 0x30;          // UDataTable::RowMap, TMap<FName, uint8*> (after RowStruct at 0x28)
constexpr size_t kSetElementSize = 0x18;        // TSetElement<TPair<FName, uint8*>>: pair (16) + hash next / index
constexpr size_t kAllocFlagsOffset = 0x10;      // TSparseArray::AllocationFlags (TBitArray: 4 inline dwords, then
constexpr size_t kAllocSecondary = 0x20;        //   the heap pointer, NumBits, MaxBits)
constexpr size_t kAllocNumBits = 0x28;
constexpr size_t kRowKey = 0x08;                // FUIButtonsMap: FKey Key (FName + TSharedPtr<FKeyDetails>)
constexpr size_t kRowImage = 0x20;              //   TSoftObjectPtr<UTexture2D> Image: weak ptr (8) + FSoftObjectPath
constexpr size_t kRowHudImage = 0x48;           //   TSoftObjectPtr<UTexture2D> HudImage

bool Allocated(uint8_t* map, int32_t i) {
    int32_t bits = *reinterpret_cast<int32_t*>(map + kAllocNumBits);
    if (i >= bits) return false;
    auto* words = bits <= 128 ? reinterpret_cast<uint32_t*>(map + kAllocFlagsOffset)
                              : *reinterpret_cast<uint32_t**>(map + kAllocSecondary);
    return (words[i / 32] >> (i % 32)) & 1;
}

uint8_t* FindRow(uint8_t* table, const ue::FName& row) {
    uint8_t* map = table + kRowMapOffset;
    auto* data = *reinterpret_cast<uint8_t**>(map);
    int32_t num = *reinterpret_cast<int32_t*>(map + 8);
    for (int32_t i = 0; i < num; ++i) {
        if (!Allocated(map, i)) continue;
        uint8_t* e = data + i * kSetElementSize;
        if (*reinterpret_cast<ue::FName*>(e) == row) return *reinterpret_cast<uint8_t**>(e + 8);
    }
    return nullptr;
}

void SetSoftTexture(uint8_t* softPtr, const std::wstring& objectPath) {
    // TSoftObjectPtr = FWeakObjectPtr (index, serial) + FSoftObjectPath (package FName, asset FName, sub path)
    auto dot = objectPath.rfind(L'.');
    std::wstring package = dot == std::wstring::npos ? objectPath : objectPath.substr(0, dot);
    std::wstring asset = dot == std::wstring::npos ? objectPath.substr(objectPath.rfind(L'/') + 1) : objectPath.substr(dot + 1);
    *reinterpret_cast<uint64_t*>(softPtr) = 0;                         // the cached object: invalid -> resolved anew
    *reinterpret_cast<ue::FName*>(softPtr + 0x08) = Name(package);
    *reinterpret_cast<ue::FName*>(softPtr + 0x10) = Name(asset);
}

}  // namespace

namespace ui {

void Init(void* fnameCtor) { g_fnameCtor = reinterpret_cast<FNameCtorFn>(fnameCtor); }

// SetUIKey|<table address hex>|<row>|<key>[|<icon texture object path>]
void SetUIKey(const std::wstring& tableHex, const std::wstring& row, const std::wstring& key, const std::wstring& icon) {
    if (!g_fnameCtor) return;
    auto* table = reinterpret_cast<uint8_t*>(std::wcstoull(tableHex.c_str(), nullptr, 16));
    if (!table || IsBadReadPtr(table, kRowMapOffset + 0x40)) {
        logx::Line(L"[ui] SetUIKey: bad table address %s", tableHex.c_str());
        return;
    }
    uint8_t* r = FindRow(table, Name(row));
    if (!r) {
        logx::Line(L"[ui] SetUIKey: no row %s", row.c_str());
        return;
    }
    if (!key.empty()) {   // an empty key: only the icon changes
        *reinterpret_cast<ue::FName*>(r + kRowKey) = Name(key);
        // the key's cached details belong to the old key: drop them (looked up again on demand; one reference leaks)
        *reinterpret_cast<void**>(r + kRowKey + 0x08) = nullptr;
        *reinterpret_cast<void**>(r + kRowKey + 0x10) = nullptr;
    }
    if (!icon.empty()) {
        SetSoftTexture(r + kRowImage, icon);
        SetSoftTexture(r + kRowHudImage, icon);
    }
    logx::Line(L"[ui] %s -> %s%s%s", row.c_str(), key.c_str(), icon.empty() ? L"" : L", icon ", icon.c_str());
}

// the address of a row's field (row data + offset hex) as hex, for dev::Watch; empty when not found
std::wstring RowAddressHex(const std::wstring& tableHex, const std::wstring& row, const std::wstring& offsetHex) {
    if (!g_fnameCtor) return L"";
    auto* table = reinterpret_cast<uint8_t*>(std::wcstoull(tableHex.c_str(), nullptr, 16));
    if (!table || IsBadReadPtr(table, kRowMapOffset + 0x40)) return L"";
    uint8_t* r = FindRow(table, Name(row));
    if (!r) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%llX", reinterpret_cast<unsigned long long>(r + std::wcstoull(offsetHex.c_str(), nullptr, 16)));
    return buf;
}

}  // namespace ui
