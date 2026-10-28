// Text overrides: replace any entry of the game's string table ("SwtStrings", loaded from
// Gameplay/Strings/NonAsset/Strings_<LANG>.csv) at runtime. Cat names are entries too (STR_CAT_NAME<n><suffix id>,
// e.g. STR_CAT_NAME1_PAW = "Woodpaw"), so this is what free-form names are built on - and any other text fix.
//
// Lua calls it through WaCaNative.Command("SetText|<key>|<text>") (commands.cpp).
#include "text.h"

#include <windows.h>

#include <string>
#include <vector>

#include "log.h"
#include "scan.h"
#include "ue.h"

namespace {

// 2026-10-24 build addresses in comments; found by signature at runtime
// One distinctive call sequence gives all four functions: UKismetStringTableLibrary::GetTableEntrySourceString's
// native thunk calls them back to back (2026-10-24 build, at 0x144366A93):
//   +0x00 FStringTableRegistry::Get()                                       0x141A13A40
//   +0x0F FStringTableRegistry::FindStringTable(out ptr, FName TableId)     0x141A11F00
//   +0x25 FTextKey::FTextKey(const FString&)                                0x141A01690
//   +0x35 FStringTable::GetSourceString(const FTextKey&, FString& Out)      0x141A17E90
constexpr const char* kGetEntrySourceThunkSig =
    "E8 ?? ?? ?? ?? 4C 8B C3 48 8D 55 F0 48 8B C8 E8 ?? ?? ?? ?? 48 8B 5D F0 48 85 DB 74 1D 48 8D 55 E0 48 8D 4D 28 E8 "
    "?? ?? ?? ?? 4C 8D 45 D0 48 8B CB 48 8D 55 28 E8 ?? ?? ?? ??";

using TextKeyCtorFn = void*(__fastcall*)(uint32_t* outKey, const ue::FString* str);
using RegistryGetFn = void*(__fastcall*)();
struct SharedPtr { void* Object; void* Controller; };
using FindTableFn = SharedPtr*(__fastcall*)(void* registry, SharedPtr* out, ue::FName tableId);
using GetSourceStringFn = bool(__fastcall*)(void* table, const uint32_t* key, ue::FString* out);
using FNameCtorFn = void(__fastcall*)(ue::FName* out, const wchar_t* name, int findType);
// TMap<FTextKey, FStringTableEntryPtr>::Find -> the pair (key, then the entry TSharedPtr at +8); GetSourceString's
// first call after taking the table's lock (+0x31)
using MapFindFn = uint8_t*(__fastcall*)(void* map, const uint32_t* key);
// FString::operator=(const FString&): GetSourceString's copy of the entry (+0x58); the engine (re)allocates
using StringAssignFn = ue::FString*(__fastcall*)(ue::FString* self, const ue::FString* other);

TextKeyCtorFn g_textKeyCtor = nullptr;
RegistryGetFn g_registryGet = nullptr;
FindTableFn g_findTable = nullptr;
GetSourceStringFn g_getSource = nullptr;
MapFindFn g_mapFind = nullptr;
StringAssignFn g_assign = nullptr;
FNameCtorFn g_fnameCtor = nullptr;
ue::FName g_tableId{};

void DumpEntry(uint8_t* entry) {
    for (int off = 0; off < 0x60; off += 8) {
        auto v = *reinterpret_cast<uint64_t*>(entry + off);
        std::wstring as;
        if (v > 0x10000 && v < 0x7FFFFFFFFFFF && !IsBadReadPtr(reinterpret_cast<void*>(v), 16)) {
            auto* w = reinterpret_cast<const wchar_t*>(v);
            for (int c = 0; c < 24 && w[c] >= 32 && w[c] < 0x2000; ++c) as += w[c];
        }
        logx::Line(L"[text]   entry+%02X = %016llX %s", off, v, as.c_str());
    }
}

}  // namespace

namespace text {

void SetText(const std::wstring& key, const std::wstring& text) {
    // FNames only exist once the engine is up: UE4SS starts C++ mods before that, so the table's name is made here
    if (g_tableId.ComparisonIndex == 0) g_fnameCtor(&g_tableId, L"SwtStrings", 1);
    void* registry = g_registryGet();
    SharedPtr table{};
    g_findTable(registry, &table, g_tableId);
    if (!table.Object) {
        logx::Line(L"[text] SetText %s: the string table is not loaded yet", key.c_str());
        return;
    }
    auto* t = static_cast<uint8_t*>(table.Object);
    // the key as an FTextKey
    ue::FString keyStr{const_cast<wchar_t*>(key.c_str()), static_cast<int32_t>(key.size() + 1), static_cast<int32_t>(key.size() + 1)};
    uint32_t textKey = 0;
    g_textKeyCtor(&textKey, &keyStr);
    ue::FString current;
    bool found = g_getSource(t, &textKey, &current);
    logx::Line(L"[text] SetText %s -> '%s' (now '%s', found %d)", key.c_str(), text.c_str(), current.c_str(), found);
    if (!found) return;
    // FStringTableEntry: +0x00 owner table (weak), +0x10 SourceString, +0x20 its text id (namespace, key)
    auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(t + 0x70);   // the table's FCriticalSection (GetSourceString's lock)
    EnterCriticalSection(cs);
    uint8_t* pair = g_mapFind(t + 0x20, &textKey);
    if (pair) {
        auto* entry = *reinterpret_cast<uint8_t**>(pair + 8);
        ue::FString value{const_cast<wchar_t*>(text.c_str()), static_cast<int32_t>(text.size() + 1), static_cast<int32_t>(text.size() + 1)};
        g_assign(reinterpret_cast<ue::FString*>(entry + 0x10), &value);
    }
    LeaveCriticalSection(cs);
    logx::Line(L"[text] %s = '%s'", key.c_str(), text.c_str());
    (void)text;
}

bool Install(void* fnameCtor) {
    g_fnameCtor = reinterpret_cast<FNameCtorFn>(fnameCtor);
    auto thunk = scan::Find(kGetEntrySourceThunkSig);
    logx::Line(L"[text] GetTableEntrySourceString calls %p", thunk.value_or(nullptr));
    if (!thunk || !g_fnameCtor) {
        logx::Line(L"[text] a signature was not found - text overrides are OFF");
        return false;
    }
    uint8_t* c = *thunk;
    g_registryGet = reinterpret_cast<RegistryGetFn>(scan::RelTarget(c));
    g_findTable = reinterpret_cast<FindTableFn>(scan::RelTarget(c + 15));
    g_textKeyCtor = reinterpret_cast<TextKeyCtorFn>(scan::RelTarget(c + 0x25));
    g_getSource = reinterpret_cast<GetSourceStringFn>(scan::RelTarget(c + 0x35));
    auto* gs = reinterpret_cast<uint8_t*>(g_getSource);
    if (gs[0x31] != 0xE8) {
        logx::Line(L"[text] GetSourceString's layout changed - text overrides are OFF");
        return false;
    }
    if (gs[0x58] != 0xE8) {
        logx::Line(L"[text] GetSourceString's layout changed - text overrides are OFF");
        return false;
    }
    g_mapFind = reinterpret_cast<MapFindFn>(scan::RelTarget(gs + 0x31));
    g_assign = reinterpret_cast<StringAssignFn>(scan::RelTarget(gs + 0x58));
    logx::Line(L"[text] text overrides active (registry %p, FindStringTable %p, GetSourceString %p)", g_registryGet, g_findTable, g_getSource);
    return true;
}

}  // namespace text
