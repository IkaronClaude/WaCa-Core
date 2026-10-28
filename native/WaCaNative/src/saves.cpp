// The game's saves as the Lua event GameSaved(nil, context). The game saves through USwtSaveManager::Save from
// native code (111 call sites) - not through ProcessEvent, so a Lua RegisterHook / WaCa.Watch on the Save UFunction
// never sees them (VM 2026-10-25: WrcGame.sav written on entering the world, the Lua hook silent). Mods that keep
// per-save data (WaCa ModKit's sidecar) write it on this event, right after the game's own save.
// Anchors (relocation framework): the Save UFunction by name -> its native exec thunk, which ends by calling the
// method (after setne r8b: the bNextTickSave argument) - the call target must be a function start.
#include "saves.h"

#include <windows.h>

#include <cstdint>
#include <string>


#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "MinHook.h"
#include "bridge.h"
#include "log.h"
#include "scan.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

// an FString's buffer: { TCHAR* data, int32 num (incl. the terminator), int32 max }
struct StrRep {
    const wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

// static: USwtSaveManager::Save(const FString& Context, bool bAsyncSave, bool bNextTickSave) - no object (the
// thunk passes the context in rcx; reading rdx as a string crashed on 0x1, VM 2026-10-25)
using SaveFn = void(__fastcall*)(const StrRep* context, bool async, bool nextTick);
SaveFn g_save = nullptr;

void __fastcall SaveDetour(const StrRep* context, bool async, bool nextTick) {
    g_save(context, async, nextTick);
    std::string ctx;
    if (context && context->Data && context->Num > 1)
        for (int32_t i = 0; i < context->Num - 1; ++i) ctx += static_cast<char>(context->Data[i] < 128 ? context->Data[i] : '?');
    bridge::Emit("GameSaved", {ctx});
}

}  // namespace

namespace saves {

void Install() {
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/SwtEngine.SwtSaveManager:Save"));
    auto* thunk = fn ? reinterpret_cast<uint8_t*>(fn->GetFunc()) : nullptr;
    if (!thunk) {
        logx::Line(L"[saves] SwtSaveManager:Save not found - no GameSaved event (sidecars are written only on the Lua path)");
        return;
    }
    // the method: the call right after `setne r8b` in the thunk's first 0x200 bytes
    uint8_t* target = nullptr;
    auto at = scan::Parse("41 0F 95 C0 E8");
    for (uint8_t* p = thunk; p < thunk + 0x200 && !target; ++p) {
        bool ok = true;
        for (size_t k = 0; k < at.Bytes.size() && ok; ++k) ok = p[k] == at.Bytes[k];
        if (ok) target = scan::RelTarget(p + 4);
    }
    if (!target || scan::FunctionStart(target) != target) {
        logx::Line(L"[saves] Save thunk at %p: no call to the save method found - anchors disagree, no GameSaved event", thunk);
        return;
    }
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&SaveDetour), reinterpret_cast<void**>(&g_save)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    logx::Line(L"[saves] save method at %p (confirmed: called by the Save UFunction's thunk %p)%s", target, thunk,
               ok ? L"" : L" - could not hook");
}

}  // namespace saves
