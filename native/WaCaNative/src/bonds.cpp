// Bonds with the story cats (tickets.md 5: activities build bonds). The game keeps a bond per narrative role (the
// save's GameplayProgression.Bonding) in an unreflected map; its own function AddPersonalBonding(role, amount) - a
// free function, rcx = the ENarrativeRole value, edx = the points - reads the current bond, adds, sets it through the
// progression object's virtual +0x560 and saves with the context "AddPersonalBonding". Found by that string (the only
// function using it) + its prologue; the second anchor: its body calls exactly that setter (call qword ptr
// [rax+560h]). tools/check_hooks.py checks the first offline. Game thread only (it saves).
#include "bonds.h"

#include <cstring>

#include "log.h"
#include "scan.h"

namespace bonds {
namespace {

using AddFn = void (*)(uint8_t role, int32_t amount);
AddFn g_add = nullptr;
bool g_tried = false;

bool Resolve() {
    if (g_tried) return g_add != nullptr;
    g_tried = true;
    uint8_t* fn = scan::FunctionUsing("AddPersonalBonding", "48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8B EC");
    if (!fn) {
        logx::Line(L"[bonds] AddPersonalBonding not found (game update?) - bond gain off");
        return false;
    }
    // the setter call within its body
    static const uint8_t setter[] = {0xFF, 0x90, 0x60, 0x05, 0x00, 0x00};
    bool seen = false;
    for (size_t i = 0; i + sizeof(setter) < 0x300 && !seen; ++i) seen = std::memcmp(fn + i, setter, sizeof(setter)) == 0;
    if (!seen) {
        logx::Line(L"[bonds] AddPersonalBonding's anchors disagree (no call [rax+560h]) - bond gain off");
        return false;
    }
    g_add = reinterpret_cast<AddFn>(fn);
    logx::Line(L"[bonds] AddPersonalBonding at %p", fn);
    return true;
}

}  // namespace

bool AddPersonal(uint8_t role, int32_t amount) {
    if (!Resolve()) return false;
    g_add(role, amount);
    logx::Line(L"[bonds] +%d bond with narrative role %u", amount, role);
    return true;
}

}  // namespace bonds
