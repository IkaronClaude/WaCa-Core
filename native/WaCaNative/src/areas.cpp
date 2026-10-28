// Movement-modifier areas (ASwtAdventureMovementModifierArea: the game's Loud / Hiding trigger boxes, and the boxes
// fishing.lua places on fishing spots) entered / left as the Lua event AreaOverlap(nil, area full name, actor full
// name, "1" entering / "0" leaving, the area's MovementFlag). Their overlap UFunctions never go through ProcessEvent
// (a WaCa.Watch sees nothing), and UE4SS's native RegisterHook on them crashed the game on world load (VM,
// 2026-10-27) - so the implementations are detoured here, after the game's own handling.
// The UFunctions' exec thunks call the area's VIRTUALS: OnActorOverlapBegin vtable +0x760, OnActorOverlapEnd +0x768
// (mov r10, [rax+760h] / [rax+768h]; call r10) - hooked from a live area's vtable (Blueprint areas share the native
// class's code). Anchors: each thunk (from its UFunction by name) loads exactly its slot; tools/check_hooks.py checks
// the same offline.
#include "areas.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "MinHook.h"
#include "bridge.h"
#include "log.h"
#include "scan.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

using BeginFn = void(__fastcall*)(void* area, void* comp, void* other, void* otherComp, int32_t body, bool sweep, const void* hit);
using EndFn = void(__fastcall*)(void* area, void* comp, void* other, void* otherComp, int32_t body);
BeginFn g_begin = nullptr;
EndFn g_end = nullptr;
bool g_tried = false;
constexpr int kBeginSlot = 0x760;
constexpr int kEndSlot = 0x768;

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void Report(void* areaPtr, void* otherPtr, bool entering) {
    auto* area = static_cast<UObject*>(areaPtr);
    auto* other = static_cast<UObject*>(otherPtr);
    static int logged = 0;
    if (logged < 5) {
        ++logged;
        logx::Line(L"[areas] %s %s by %s", entering ? L"begin" : L"end", area ? area->GetName().c_str() : L"?",
                   other ? other->GetName().c_str() : L"?");
    }
    if (!area || !other) return;
    auto* flag = area->GetValuePtrByPropertyNameInChain<int32_t>(STR("MovementFlag"));
    bridge::Emit("AreaOverlap", {Narrow(area->GetFullName()), Narrow(other->GetFullName()), entering ? "1" : "0",
                                 std::to_string(flag ? *flag : -1)});
}

void __fastcall BeginDetour(void* area, void* comp, void* other, void* otherComp, int32_t body, bool sweep, const void* hit) {
    g_begin(area, comp, other, otherComp, body, sweep, hit);
    Report(area, other, true);
}

void __fastcall EndDetour(void* area, void* comp, void* other, void* otherComp, int32_t body) {
    g_end(area, comp, other, otherComp, body);
    Report(area, other, false);
}

uint8_t* Thunk(const wchar_t* path) {
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    return fn ? reinterpret_cast<uint8_t*>(fn->GetFunc()) : nullptr;
}

}  // namespace

namespace areas {

bool HookFrom(void* area) {
    if (g_tried || !area) return g_begin != nullptr;
    g_tried = true;
    auto* beginThunk = Thunk(STR("/Script/SwtAdventure.SwtAdventureMovementModifierArea:OnActorOverlapBegin"));
    auto* endThunk = Thunk(STR("/Script/SwtAdventure.SwtAdventureMovementModifierArea:OnActorOverlapEnd"));
    if (!beginThunk || !endThunk || !scan::BodyContains(beginThunk, "4C 8B 90 60 07 00 00", 1) ||
        !scan::BodyContains(endThunk, "4C 8B 90 68 07 00 00", 1)) {
        logx::Line(L"[areas] the overlap thunks %p / %p don't load slots +0x%X / +0x%X - anchors disagree, area events OFF",
                   beginThunk, endThunk, kBeginSlot, kEndSlot);
        return false;
    }
    auto** vtable = *reinterpret_cast<uint8_t***>(area);
    bool ok = MH_CreateHook(vtable[kBeginSlot / 8], reinterpret_cast<void*>(&BeginDetour), reinterpret_cast<void**>(&g_begin)) == MH_OK &&
              MH_CreateHook(vtable[kEndSlot / 8], reinterpret_cast<void*>(&EndDetour), reinterpret_cast<void**>(&g_end)) == MH_OK &&
              MH_EnableHook(vtable[kBeginSlot / 8]) == MH_OK && MH_EnableHook(vtable[kEndSlot / 8]) == MH_OK;
    logx::Line(L"[areas] overlap begin %p / end %p (confirmed: the thunks load their slots)%s", vtable[kBeginSlot / 8],
               vtable[kEndSlot / 8], ok ? L"" : L" - could not hook");
    if (!ok) g_begin = nullptr;
    return ok;
}

}  // namespace areas
