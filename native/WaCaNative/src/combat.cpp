// The player's combat strength by specialisation (tickets.md 13: a Fighter hits harder and takes less). Every hit in
// the game's turn-based combat is queued through the combat director's UFunction OnRegisterDamage(
// FSwtAdventureReceiveDamageStruct) - reflected, so it passes ProcessEvent: a pre callback scales the struct's Damage
// before the director queues it. The hit's receiver is the struct's Character; the attacker is the director's
// CurrentTurnAttackCharacter; the player's own cat is its MainPlayerCharacter (party allies are left as they are).
// Every offset comes from reflection (the parameter, the struct's fields, the director's properties) - nothing baked.
// Configured from Lua (CombatScale|dealt|taken, roles.ini via hunting.lua); 1|1 = untouched.
#include "combat.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>

#include "log.h"

using namespace RC;
using namespace RC::Unreal;

namespace combat {
namespace {

std::atomic<double> g_dealt{1.0};
std::atomic<double> g_taken{1.0};
int g_logged = 0;

struct Layout {
    bool tried = false;
    bool ok = false;
    FName function;
    UClass* director = nullptr;
    int32_t param = 0;         // the struct parameter in the call's parameters
    int32_t character = 0;     // its Character (the receiver)
    int32_t damage = 0;        // its Damage (int32)
    int32_t missed = 0;        // its bMissed
};
Layout g_layout;

bool Resolve() {
    if (g_layout.tried) return g_layout.ok;
    g_layout.tried = true;
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/SwtAdventure.SwtAdventureCombatDirector:OnRegisterDamage"));
    g_layout.director = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/SwtAdventure.SwtAdventureCombatDirector"));
    if (!fn || !g_layout.director) {
        logx::Line(L"[combat] OnRegisterDamage not found - combat scaling off");
        return false;
    }
    auto* param = static_cast<FStructProperty*>(fn->FindProperty(FName(STR("receiveDamageStruct"), FNAME_Find)));
    UScriptStruct* st = param ? param->GetStruct().Get() : nullptr;
    auto* character = st ? st->FindProperty(FName(STR("Character"), FNAME_Find)) : nullptr;
    auto* damage = st ? st->FindProperty(FName(STR("Damage"), FNAME_Find)) : nullptr;
    auto* missed = st ? st->FindProperty(FName(STR("bMissed"), FNAME_Find)) : nullptr;
    if (!character || !damage || !missed) {
        logx::Line(L"[combat] the damage struct's fields not found - combat scaling off");
        return false;
    }
    g_layout.function = fn->GetNamePrivate();
    g_layout.param = param->GetOffset_Internal();
    g_layout.character = character->GetOffset_Internal();
    g_layout.damage = damage->GetOffset_Internal();
    g_layout.missed = missed->GetOffset_Internal();
    g_layout.ok = true;
    logx::Line(L"[combat] OnRegisterDamage: struct at +%d (Character +%d, Damage +%d)", g_layout.param, g_layout.character,
               g_layout.damage);
    return true;
}

void OnPre(UObject* context, UFunction* function, void* parms) {
    const double dealt = g_dealt.load(std::memory_order_relaxed), taken = g_taken.load(std::memory_order_relaxed);
    if ((dealt == 1.0 && taken == 1.0) || !context || !function || !parms) return;
    if (!g_layout.ok && (g_layout.tried || !Resolve())) return;
    if (!(function->GetNamePrivate() == g_layout.function) || !context->IsA(g_layout.director)) return;
    auto* hit = static_cast<uint8_t*>(parms) + g_layout.param;
    const bool miss = *reinterpret_cast<bool*>(hit + g_layout.missed);
    auto** attacker = context->GetValuePtrByPropertyNameInChain<UObject*>(STR("CurrentTurnAttackCharacter"));
    auto** player = context->GetValuePtrByPropertyNameInChain<UObject*>(STR("MainPlayerCharacter"));
    UObject* receiver = *reinterpret_cast<UObject**>(hit + g_layout.character);
    auto* amount = reinterpret_cast<int32_t*>(hit + g_layout.damage);
    static int seen = 0;
    if (seen < 30) {
        // every hit's facts (the first 30 of a session): who hit whom, a miss, the damage - so a hit that isn't scaled
        // shows why
        ++seen;
        logx::Line(L"[combat] hit: attacker is your cat %d, receiver is your cat %d, missed %d, damage %d",
                   attacker && player && *attacker == *player ? 1 : 0, player && receiver == *player ? 1 : 0, miss ? 1 : 0,
                   *amount);
    }
    if (miss || !player || !*player) return;
    double factor = 1.0;
    const wchar_t* who = nullptr;
    if (attacker && *attacker == *player && receiver != *player) { factor = dealt; who = L"dealt"; }
    else if (receiver == *player) { factor = taken; who = L"taken"; }
    if (!who || factor == 1.0) return;
    const int32_t before = *amount;
    *amount = static_cast<int32_t>(std::lround(before * factor));
    if (g_logged < 20) {
        ++g_logged;
        logx::Line(L"[combat] damage %s by your cat: %d -> %d (x%.2f)", who, before, *amount, factor);
    }
}

}  // namespace

void Install() {
    Hook::RegisterProcessEventPreCallback([](auto&, UObject* c, UFunction* f, void* p) { OnPre(c, f, p); },
                                          {false, true, STR("WaCaCore"), STR("CombatScale")});
}

void Configure(const std::wstring& command) {
    // CombatScale|dealt|taken
    size_t a = command.find(L'|'), b = a == std::wstring::npos ? a : command.find(L'|', a + 1);
    if (b == std::wstring::npos) return;
    double dealt = _wtof(command.substr(a + 1, b - a - 1).c_str()), taken = _wtof(command.substr(b + 1).c_str());
    if (dealt <= 0) dealt = 1.0;
    if (taken <= 0) taken = 1.0;
    g_dealt = dealt;
    g_taken = taken;
    g_logged = 0;
    logx::Line(L"[combat] your cat's damage dealt x%.2f, taken x%.2f", dealt, taken);
}

}  // namespace combat
