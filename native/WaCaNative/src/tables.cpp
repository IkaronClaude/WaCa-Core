// Rows added to the game's own data tables (a mod's pattern / marking becomes an entry in the creator's grids).
//
// UE4SS can add a row, but copies the row struct with its ICppStructOps::Copy, whose place UE4SS doesn't know on this
// engine version ("Virtual UScriptStruct::ICppStructOps::Copy is unavailable"). So rows are cloned here, property by
// property from the row struct's reflection: plain values as bytes, strings deep (the engine's allocator), soft object
// pointers with their path's string deep, arrays of those element by element, nested structs recursively, maps with
// numeric keys rebuilt pair by pair (rehashed). Other maps and sets start empty (logged).

//
// Fields are then set by name (SetField): text (FString), soft object paths ("/Game/Path/Asset.Asset"), enum /
// integer / bool values.
#include "tables.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/Core/Containers/Set.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>
#include <Unreal/UScriptStruct.hpp>

#include <cstring>
#include <string>

#include "log.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

// an FString's buffer: TArray<TCHAR> { Data, Num (incl. terminator), Max }
struct StrRep {
    TCHAR* Data;
    int32 Num;
    int32 Max;
};

struct ArrRep {
    uint8* Data;
    int32 Num;
    int32 Max;
};

// a fresh engine-allocated copy of a string (the field already holds the source's bytes, shallowly)
void DeepString(void* field) {
    auto* s = static_cast<StrRep*>(field);
    if (!s->Data || s->Num <= 0) {
        *s = {nullptr, 0, 0};
        return;
    }
    auto* copy = static_cast<TCHAR*>(FMemory::Malloc(sizeof(TCHAR) * s->Num));
    std::memcpy(copy, s->Data, sizeof(TCHAR) * s->Num);
    s->Data = copy;
    s->Max = s->Num;
}

// a soft object pointer (FWeakObjectPtr 8 + FSoftObjectPath: two FNames 16 + the sub path FString 16)
constexpr int32 kSoftSubPath = 8 + 16;

void DeepValue(FProperty* p, uint8* value, bool& lossy);

// numeric keys hash to their value in the engine (GetTypeHash of an integer / enum)
bool NumericKey(FProperty* key) {
    return CastField<FByteProperty>(key) || CastField<FEnumProperty>(key) || CastField<FIntProperty>(key);
}

// A map rebuilt element by element through the engine's own map layout (UE4SS FScriptMap): each pair's key and value
// copied (deeply), then the hash rebuilt. Only maps with numeric keys - other key types hash in ways not worth
// re-implementing; they start empty (logged as lossy). `value` holds the source map's bytes (a shallow copy).
void CopyMap(FMapProperty* mp, uint8* value, bool& lossy) {
    FProperty* keyProp = mp->GetKeyProp();
    FProperty* valueProp = mp->GetValueProp();
    const FScriptMapLayout& layout = mp->GetMapLayout();
    alignas(FScriptMap) uint8 srcBytes[sizeof(FScriptMap)];
    std::memcpy(srcBytes, value, sizeof(FScriptMap));                  // a view of the source - never destructed
    auto* src = reinterpret_cast<FScriptMap*>(srcBytes);
    auto* dst = new (value) FScriptMap();
    if (!NumericKey(keyProp)) {
        lossy = true;
        return;
    }
    int32 keySize = keyProp->GetElementSize();
    for (int32 i = 0; i < src->GetMaxIndex(); ++i) {
        if (!src->IsValidIndex(i)) continue;
        auto* from = static_cast<const uint8*>(src->GetData(i, layout));
        int32 n = dst->AddUninitialized(layout);
        auto* to = static_cast<uint8*>(dst->GetData(n, layout));
        std::memcpy(to, from, keySize);
        std::memcpy(to + layout.ValueOffset, from + layout.ValueOffset, valueProp->GetElementSize());
        DeepValue(keyProp, to, lossy);
        DeepValue(valueProp, to + layout.ValueOffset, lossy);
    }
    dst->Rehash(layout, [keySize](const void* key) {
        uint64 v = 0;
        std::memcpy(&v, key, keySize);
        return static_cast<uint32>(v);
    });
}

void DeepStruct(UStruct* st, uint8* data, bool& lossy) {
    for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeDeprecated)) {
        for (int32 i = 0; i < p->GetArrayDim(); ++i) DeepValue(p, data + p->GetOffset_Internal() + i * p->GetElementSize(), lossy);
    }
}

void DeepValue(FProperty* p, uint8* value, bool& lossy) {
    if (CastField<FStrProperty>(p)) {
        DeepString(value);
    } else if (CastField<FSoftObjectProperty>(p)) {
        DeepString(value + kSoftSubPath);
    } else if (auto* sp = CastField<FStructProperty>(p)) {
        DeepStruct(sp->GetStruct(), value, lossy);
    } else if (auto* ap = CastField<FArrayProperty>(p)) {
        auto* a = reinterpret_cast<ArrRep*>(value);
        FProperty* inner = ap->GetInner();
        int32 size = inner->GetElementSize();
        if (!a->Data || a->Num <= 0) {
            *a = {nullptr, 0, 0};
            return;
        }
        auto* copy = static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(size) * a->Num));
        std::memcpy(copy, a->Data, static_cast<size_t>(size) * a->Num);
        a->Data = copy;
        a->Max = a->Num;
        for (int32 i = 0; i < a->Num; ++i) DeepValue(inner, copy + static_cast<size_t>(i) * size, lossy);
    } else if (auto* mp = CastField<FMapProperty>(p)) {
        CopyMap(mp, value, lossy);
    } else if (CastField<FSetProperty>(p)) {
        new (value) FScriptSet();                          // empty (see the file comment)
        lossy = true;
    }
    // everything else (numbers, enums, bools, names, object pointers) is plain bytes, already copied
}

UScriptStruct* RowStructOf(UDataTable* table) { return table->GetRowStruct().Get(); }

// an enum value by name ("P13_Tabby" or "EPatternType::P13_Tabby"), else the text as a number
int64 EnumValue(UEnum* e, const std::wstring& name) {
    if (e) {
        for (auto& pair : e->GetEnumNames()) {
            std::wstring full = pair.Key.ToString();
            size_t sep = full.rfind(L"::");
            std::wstring shortName = sep == std::wstring::npos ? full : full.substr(sep + 2);
            if (full == name || shortName == name) return pair.Value;
        }
    }
    return _wtoi64(name.c_str());
}

uint8* FindRow(UDataTable* table, const std::wstring& name) {
    FName key(name.c_str(), FNAME_Find);
    for (auto& pair : table->GetRowMap()) {
        if (pair.Key == key) return pair.Value;
    }
    return nullptr;
}

FProperty* FindField(UScriptStruct* st, const std::wstring& name) {
    for (FProperty* p : TFieldRange<FProperty>(st, EFieldIterationFlags::IncludeDeprecated)) {
        if (p->GetName() == name) return p;
    }
    return nullptr;
}

}  // namespace

namespace tables {

bool CloneRow(void* tablePtr, const std::wstring& from, const std::wstring& to) {
    auto* table = static_cast<UDataTable*>(tablePtr);
    UScriptStruct* st = table ? RowStructOf(table) : nullptr;
    uint8* src = st ? FindRow(table, from) : nullptr;
    if (!src) {
        logx::Line(L"[tables] clone %s -> %s: %s", from.c_str(), to.c_str(), st ? L"no such row" : L"no row struct");
        return false;
    }
    if (FindRow(table, to)) {
        logx::Line(L"[tables] clone %s -> %s: the row exists already", from.c_str(), to.c_str());
        return true;
    }
    int32 size = st->GetStructureSize();
    auto* row = static_cast<uint8*>(FMemory::Malloc(size));
    std::memcpy(row, src, size);
    bool lossy = false;
    DeepStruct(st, row, lossy);
    table->GetRowMap().Add(FName(to.c_str(), FNAME_Add), row);
    logx::Line(L"[tables] clone %s -> %s: added (%d bytes)%s", from.c_str(), to.c_str(), size,
               lossy ? L", maps / sets left empty" : L"");
    return true;
}

bool SetField(void* tablePtr, const std::wstring& rowName, const std::wstring& field, const std::wstring& value) {
    auto* table = static_cast<UDataTable*>(tablePtr);
    UScriptStruct* st = table ? RowStructOf(table) : nullptr;
    uint8* row = st ? FindRow(table, rowName) : nullptr;
    FProperty* p = row ? FindField(st, field) : nullptr;
    if (!p) {
        logx::Line(L"[tables] set %s.%s: no such %s", rowName.c_str(), field.c_str(), row ? L"field" : L"row");
        return false;
    }
    uint8* v = row + p->GetOffset_Internal();
    if (CastField<FStrProperty>(p)) {
        // the old buffer is the engine's: leaked deliberately (a few bytes per mod row) rather than freed by hand
        auto* s = reinterpret_cast<StrRep*>(v);
        int32 n = static_cast<int32>(value.size()) + 1;
        auto* buf = static_cast<TCHAR*>(FMemory::Malloc(sizeof(TCHAR) * n));
        std::memcpy(buf, value.c_str(), sizeof(TCHAR) * n);
        *s = {buf, n, n};
    } else if (CastField<FSoftObjectProperty>(p)) {
        // "/Game/Folder/Asset.Asset" -> package name + asset name; no weak pointer (resolved on load)
        size_t dot = value.rfind(L'.');
        std::wstring package = dot == std::wstring::npos ? value : value.substr(0, dot);
        std::wstring asset = dot == std::wstring::npos ? value.substr(value.rfind(L'/') + 1) : value.substr(dot + 1);
        std::memset(v, 0, 8);                                                    // weak pointer: none
        *reinterpret_cast<FName*>(v + 8) = FName(package.c_str(), FNAME_Add);
        *reinterpret_cast<FName*>(v + 16) = FName(asset.c_str(), FNAME_Add);
        *reinterpret_cast<StrRep*>(v + kSoftSubPath) = {nullptr, 0, 0};          // no sub path
    } else if (auto* ep = CastField<FEnumProperty>(p)) {
        int64 n = EnumValue(ep->GetEnum().Get(), value);
        std::memcpy(v, &n, p->GetElementSize());
    } else if (auto* bp = CastField<FByteProperty>(p)) {
        *v = static_cast<uint8>(EnumValue(bp->GetEnum().Get(), value));
    } else if (CastField<FBoolProperty>(p)) {
        *v = (value == L"true" || value == L"1") ? 1 : 0;
    } else if (CastField<FIntProperty>(p)) {
        *reinterpret_cast<int32*>(v) = _wtoi(value.c_str());
    } else {
        logx::Line(L"[tables] set %s.%s: this field type is not supported", rowName.c_str(), field.c_str());
        return false;
    }
    logx::Line(L"[tables] %s.%s = %s", rowName.c_str(), field.c_str(), value.c_str());
    return true;
}

}  // namespace tables
