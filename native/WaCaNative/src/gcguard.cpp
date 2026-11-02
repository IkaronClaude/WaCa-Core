// A guard against an engine crash in the garbage collector's purge (operator 2026-11-02: "fix that crash as well, even
// if it's pre-existing"). EXCEPTION_ACCESS_VIOLATION writing 0x0 at exe+0x1c64787, right after a level load or a big
// teleport - on the VM ~1 boot in 5 (2026-10-29: 5 times in an afternoon; 2026-11-01 22:07; 2026-11-02 09:13), with or
// without any mod's Lua running. The crashing function is a small setter the incremental purge
// (exe+0x1c37e80 -> +0x1c38485) calls:
//     push rbx / sub rsp,20h / mov rbx,rcx
//     call A ; test rax,rax ; je skip ; call A ; mov rcx,rax ; call B
//   skip:
//     mov rax,[rip+G] ; mov [rax],rbx        <- G still null: the write to 0x0
//     add rsp,20h / pop rbx / ret
// G is a pointer the engine sets up lazily; under the VM's ~0.3 fps (WARP) the purge's time-out path gets here before
// it is. Detoured: with G set the original runs untouched; with G null, A / B run as they would and only the store is
// skipped (the unpatched game crashes right there - skipping is strictly better), logged once. The function is found
// by its whole body (unique in the exe); A, B and G are read from the code itself, not hardcoded.
#include "gcguard.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "scan.h"

namespace gcguard {
namespace {

using SetFn = void(__fastcall*)(void* value);
using AFn = void*(__fastcall*)();
using BFn = void(__fastcall*)(void*);

SetFn g_orig = nullptr;
AFn g_a = nullptr;
BFn g_b = nullptr;
void** g_slot = nullptr;              // &G
std::atomic<int> g_skipped{0};

void __fastcall Detour(void* value) {
    if (*g_slot) {
        g_orig(value);
        return;
    }
    if (void* a = g_a()) g_b(g_a());
    if (g_skipped.fetch_add(1) == 0) {
        logx::Line(L"[gcguard] the purge's setter ran before its target existed - store skipped (the game would have "
                   L"crashed here: exe+0x1c64787 writing 0x0)");
    }
}

uint8_t* RelTarget(uint8_t* at, int insnLen, int dispOffset) {
    int32_t d;
    std::memcpy(&d, at + dispOffset, sizeof d);
    return at + insnLen + d;
}

}  // namespace

void Install() {
    auto found = scan::Find(
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 0D E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
        "48 8B 05 ?? ?? ?? ?? 48 89 18 48 83 C4 20 5B C3");
    if (!found) {
        logx::Line(L"[gcguard] the purge's setter not found (game update?) - no guard");
        return;
    }
    uint8_t* fn = *found;
    g_a = reinterpret_cast<AFn>(RelTarget(fn + 9, 5, 1));
    auto* a2 = RelTarget(fn + 19, 5, 1);
    g_b = reinterpret_cast<BFn>(RelTarget(fn + 27, 5, 1));
    g_slot = reinterpret_cast<void**>(RelTarget(fn + 32, 7, 3));
    if (a2 != reinterpret_cast<uint8_t*>(g_a)) {
        logx::Line(L"[gcguard] the setter's two calls differ (%p / %p) - not the function expected, no guard", g_a, a2);
        return;
    }
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_orig)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        logx::Line(L"[gcguard] hook failed at %p - no guard", fn);
        return;
    }
    logx::Line(L"[gcguard] the purge's setter guarded at %p (its pointer at %p)", fn, g_slot);
}

}  // namespace gcguard
