// The cat look, hooked where the game builds it: the three primitives a cat's material is made of, each called per
// mesh part (body, tail, ears, fur cards) when the game (re)builds a look:
//   colour        (component, part, colour index, slot)   slot 1 Fur, 2 Pattern Mask, 3 Markings Mask, 4/5/45 eyes,
//                                                          6-44 the small parts (nose, paws, ...)
//   pattern mask  (component, part)                        the "Pattern Mask" texture from the cat's PatternIdx
//   markings mask (component, part)                        the "Markings Mask" texture from the cat's MarkingIdx
// Right after the game's own write, the Lua event CatLookPart(component, what, part name) lets mods put their own
// value on the same part, in the same call: the look is part of the game's build - no repaint afterwards, no check
// whether it is still there, no frame with the game's value showing.
// Found by the parameter names each one uses (exe strings), checked by their prologues (2026-10-25 build:
// 0x1417B1B70 / 0x1417B2D50 / 0x1417B3770). Any of them not found = that part of the hook is off, logged.
#include "appearance.h"

#include <windows.h>

#include <cstdint>
#include <string>

#include "MinHook.h"
#include "bridge.h"
#include "log.h"
#include "scan.h"

namespace {

using ColourFn = void(__fastcall*)(void* component, void* part, int32_t colourIndex, int32_t slot);
using MaskFn = void(__fastcall*)(void* component, void* part);
// the whole build of a cat's look from its data: SetDefaultCustomData(component, data, flag) (its timing scope is
// "SetDefaultCustomData Total Exe"; the save path calls it with flag 0) - what RebuildLook asks the game to do
using SetDataFn = void(__fastcall*)(void* component, const void* customData, bool flag);
SetDataFn g_setData = nullptr;

ColourFn g_colour = nullptr;
MaskFn g_patternMask = nullptr;
MaskFn g_markingsMask = nullptr;

void __fastcall ColourDetour(void* component, void* part, int32_t colourIndex, int32_t slot) {
    g_colour(component, part, colourIndex, slot);
    if (component && part) bridge::EmitLook(component, part, ("colour:" + std::to_string(slot)).c_str());
}

void __fastcall PatternMaskDetour(void* component, void* part) {
    g_patternMask(component, part);
    if (component && part) bridge::EmitLook(component, part, "PatternMask");
}

void __fastcall MarkingsMaskDetour(void* component, void* part) {
    g_markingsMask(component, part);
    if (component && part) bridge::EmitLook(component, part, "MarkingsMask");
}

// Each primitive is found twice, independently: by the string it uses (+ its prologue), and by the call graph - the
// full build (SetDefaultCustomData, anchored on its own string) reaches it within two calls. Anchors that disagree =
// the game changed under us: that part stays OFF rather than detouring the wrong function. Without the full build
// there is nothing to cross-check against: hooked on the string anchor alone, logged as unconfirmed.
bool Hook(const wchar_t* what, const char* anchor, const char* prologue, void* detour, void** original) {
    uint8_t* fn = scan::FunctionUsing(anchor, prologue);
    if (!fn) {
        logx::Line(L"[look] %s: not found (anchor \"%S\") - that part of the look hook is OFF", what, anchor);
        return false;
    }
    const wchar_t* check = L" (unconfirmed: no full build to cross-check)";
    if (g_setData) {
        if (!scan::Reaches(reinterpret_cast<uint8_t*>(g_setData), fn, 2)) {
            logx::Line(L"[look] %s at %p: NOT reached from the full build - anchors disagree, that part of the look hook is OFF",
                       what, fn);
            return false;
        }
        check = L" (confirmed: reached from the full build)";
    }
    bool ok = MH_CreateHook(fn, detour, original) == MH_OK && MH_EnableHook(fn) == MH_OK;
    logx::Line(L"[look] %s at %p%s%s", what, fn, check, ok ? L"" : L" - could not hook");
    return ok;
}

}  // namespace

namespace appearance {

void Install() {
    // test switch: WACA_NOLOOK=1 leaves the look build untouched (to tell a game / environment crash from ours)
    wchar_t off[4] = {};
    if (GetEnvironmentVariableW(L"WACA_NOLOOK", off, 4) && off[0] == L'1') {
        logx::Line(L"[look] off (WACA_NOLOOK=1)");
        return;
    }
    ULONGLONG t = GetTickCount64();
    // the full build first: the primitives are cross-checked against it
    g_setData = reinterpret_cast<SetDataFn>(scan::FunctionUsing("Fur Color", "40 55 53 57 41 56 41 57"));
    logx::Line(L"[look] full build (SetDefaultCustomData) %s %p", g_setData ? L"at" : L"NOT FOUND -", g_setData);
    Hook(L"colour", "Pattern Mask Color", "48 89 5C 24 10", reinterpret_cast<void*>(&ColourDetour),
         reinterpret_cast<void**>(&g_colour));
    Hook(L"pattern mask", "Pattern Mask", "48 85 D2 0F 84", reinterpret_cast<void*>(&PatternMaskDetour),
         reinterpret_cast<void**>(&g_patternMask));
    Hook(L"markings mask", "Markings Mask", "48 85 D2 0F 84", reinterpret_cast<void*>(&MarkingsMaskDetour),
         reinterpret_cast<void**>(&g_markingsMask));
    logx::Line(L"[look] hooks found in %llu ms", GetTickCount64() - t);
}

bool Rebuild(void* component, const void* customData) {
    if (!g_setData || !component || !customData) return false;
    g_setData(component, customData, false);
    return true;
}

}  // namespace appearance
