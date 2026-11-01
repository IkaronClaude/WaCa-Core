// The fight's party: who fights beside your cat (tickets.md "The party = who you're out with").
//
// A story fight's director (ASwtAdventureCombatDirector) keeps the party as narrative roles in its own array
// (2026-11-01 build: +0x788, TArray of {ENarrativeRole u8, FGameplayTag} = 12 bytes - the save's PartySetup). Its
// virtual at +0x9E0 empties that array and fills it from the quest data before a fight; +0x9E8 then spawns the fight
// from it (up to 3 cats: min(3, roles + 1) - your cat and two). Party|<role>,<role> (Lua: WaCa.SetCombatParty) makes
// the fill's result those roles instead - the game then builds the cats from them its own way (their looks, stats and
// moves, as for its own story parties). Only story cats (narrative roles) can be in it. Party| alone = the game's.
// The director classes that pick their party differently (Skirmish / Defender: the AnyPlayerParty director) override
// +0x9E0 and are left alone.
#include "party.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <vector>

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "MinHook.h"
#include "log.h"
#include "scan.h"
#include "ue.h"

using namespace RC;
using namespace RC::Unreal;

namespace party {
namespace {

constexpr size_t kFillSlot = 0x9E0;

struct RoleEntry {
    uint8_t Role;
    uint8_t Pad[3];
    ue::FName Tag;
};
static_assert(sizeof(RoleEntry) == 0xC);

using FillFn = void(__fastcall*)(void* director);
FillFn g_fillOrig = nullptr;
uint32_t g_rolesOffset = 0;
bool g_tried = false, g_hooked = false;
std::mutex g_mutex;
std::vector<uint8_t> g_roles;

void __fastcall FillDetour(void* director) {
    g_fillOrig(director);
    std::vector<uint8_t> roles;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        roles = g_roles;
    }
    if (roles.empty()) return;
    auto* list = reinterpret_cast<ue::TArray<RoleEntry>*>(static_cast<uint8_t*>(director) + g_rolesOffset);
    if (!list->Data || list->Num <= 0 || list->Max <= 0) {
        // (no story party to start from: the tag each entry carries and the room for it come from the game's list)
        logx::Line(L"[party] the game's party is empty this fight - the chosen party can't be put in");
        return;
    }
    ue::FName tag = list->Data[0].Tag;
    int n = std::min<int>(static_cast<int>(roles.size()), list->Max);
    std::wstring said;
    for (int i = 0; i < n; ++i) {
        list->Data[i].Role = roles[i];
        if (i >= list->Num) list->Data[i].Tag = tag;
        said += (i ? L", " : L"") + std::to_wstring(roles[i]);
    }
    list->Num = n;
    logx::Line(L"[party] this fight's party: roles %s (the game's list replaced)", said.c_str());
}

bool Hook() {
    g_tried = true;
    auto* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/SwtAdventure.Default__SwtAdventureCombatDirector"));
    if (!cdo) {
        logx::Line(L"[party] the combat director class isn't loaded - the party can't be set");
        return false;
    }
    auto* fn = (*reinterpret_cast<uint8_t***>(cdo))[kFillSlot / 8];
    // anchors: "lea rdi, [rcx + <roles>]" at +0x0A and the array's 12-byte element ("mov ecx, 0xC") in its body
    if (!(fn[0x0A] == 0x48 && fn[0x0B] == 0x8D && fn[0x0C] == 0xB9) || !scan::BodyContains(fn, "B9 0C 00 00 00", 1)) {
        logx::Line(L"[party] the director's party fill (+0x%X, %p) doesn't look as expected - the party can't be set", kFillSlot, fn);
        return false;
    }
    g_rolesOffset = *reinterpret_cast<uint32_t*>(fn + 0x0D);
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&FillDetour), reinterpret_cast<void**>(&g_fillOrig)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        logx::Line(L"[party] could not hook the director's party fill");
        return false;
    }
    logx::Line(L"[party] the director's party fill hooked (%p, roles at +0x%X)", fn, g_rolesOffset);
    g_hooked = true;
    return true;
}

}  // namespace

// Party|29,43: the next fights' party = those ENarrativeRole values (your cat + up to the game's room); Party| = the
// game's own. On the game thread (the class lookup).
bool Set(const std::wstring& list) {
    if (!g_tried) Hook();
    std::vector<uint8_t> roles;
    std::wstringstream ss(list);
    std::wstring item;
    while (std::getline(ss, item, L',')) {
        try {
            int v = std::stoi(item);
            if (v > 0 && v < 256) roles.push_back(static_cast<uint8_t>(v));
        } catch (...) {
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_roles = roles;
    }
    logx::Line(L"[party] chosen party: %s", roles.empty() ? L"the game's own" : list.c_str());
    return g_hooked;
}

}  // namespace party
