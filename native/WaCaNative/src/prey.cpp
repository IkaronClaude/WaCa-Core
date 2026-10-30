// Prey that flee (tickets.md 9, hunting rework): when a prey animal notices a cat, the game runs its reaction - the
// prey charges / an encounter starts. With PreyFlee on, that reaction is skipped for prey whose size tag isn't
// excluded, and the Lua event PreyFlee(nil, prey full name, cat full name) lets the mod make it run away; the
// game's pounce (USwtAdventurePreyComponent) still catches it.
// The reaction: AWrcPouncePrey's UFunction OnCharacterDetected(detectedCharacter) is native; its exec thunk calls
// the prey's VIRTUAL vtable +0x9F0 (rcx = prey, rdx = the cat) - hooked from a live prey's vtable. Anchors: the
// thunk (from the UFunction by name) calls exactly that slot (call qword ptr [rax+9F0h]); tools/check_hooks.py
// checks the same offline.
#include "prey.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <Unreal/NameTypes.hpp>
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

using ReactFn = void(__fastcall*)(void* prey, void* character);
ReactFn g_react = nullptr;
bool g_tried = false;
constexpr int kReactSlot = 0x9F0;

std::atomic<bool> g_on{false};
std::mutex g_mutex;
std::vector<std::wstring> g_excluded;      // size tags (DimensionsTag) that keep the game's reaction

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring SizeTag(UObject* prey) {
    auto* tag = prey->GetValuePtrByPropertyNameInChain<FName>(STR("DimensionsTag"));
    return tag ? tag->ToString() : L"";
}

void __fastcall ReactDetour(void* preyPtr, void* characterPtr) {
    auto* prey = static_cast<UObject*>(preyPtr);
    auto* cat = static_cast<UObject*>(characterPtr);
    if (!g_on || !prey || !cat) {
        g_react(preyPtr, characterPtr);
        return;
    }
    std::wstring tag = SizeTag(prey);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& t : g_excluded) {
            if (!t.empty() && tag == t) {
                g_react(preyPtr, characterPtr);
                return;
            }
        }
    }
    static std::vector<std::wstring> seenTags;
    if (std::find(seenTags.begin(), seenTags.end(), tag) == seenTags.end()) {
        seenTags.push_back(tag);
        logx::Line(L"[prey] a prey with size tag '%s' noticed a cat - it flees (hunting.ini ExcludeTags keeps the game's "
                   L"reaction for a tag)", tag.c_str());
    }
    bridge::Emit("PreyFlee", {Narrow(prey->GetFullName()), Narrow(cat->GetFullName())});
}

// SMALL PREY STARTS NO FIGHT (operator 2026-10-30: a field mouse still fought back; 2026-10-27: "catch = instant on
// touch; rats stay hostile"). Touching a prey creature starts the game's fight through its UFunction
// ASwtAdventureCharacter::OnEncounterOverlap (a dynamic delegate -> ProcessEvent -> its native exec thunk). Its encounter
// capsule's collision off didn't stop it (VM: the overlap still came). So the thunk is hooked: for the CharacterTypes Lua
// lists the call is dropped - no fight; the catch stays the pounce. Called through ProcessEvent the frame is discarded
// after the call, so not reading its parameters is safe.
using ExecFn = void(__fastcall*)(UObject* context, void* stack, void* result);
ExecFn g_encounter = nullptr;
bool g_encounterTried = false;
std::mutex g_peaceMutex;
std::set<int> g_peace;                     // CharacterType values that start no fight

void __fastcall EncounterDetour(UObject* context, void* stack, void* result) {
    if (context) {
        auto* type = context->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CharacterType"));
        if (type) {
            bool calm;
            {
                std::lock_guard<std::mutex> lock(g_peaceMutex);
                calm = g_peace.count(*type) > 0;
            }
            if (calm) {
                static UObject* lastLogged = nullptr;
                if (lastLogged != context) {
                    lastLogged = context;
                    logx::Line(L"[prey] %s (type %d) touched - no fight (small prey)", context->GetName().c_str(), *type);
                }
                return;
            }
        }
    }
    g_encounter(context, stack, result);
}

bool HookEncounter() {
    if (g_encounterTried) return g_encounter != nullptr;
    g_encounterTried = true;
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
        STR("/Script/SwtAdventure.SwtAdventureCharacter:OnEncounterOverlap"));
    auto* thunk = fn ? reinterpret_cast<void*>(fn->GetFunc()) : nullptr;
    if (!thunk) {
        logx::Line(L"[prey] OnEncounterOverlap not found - small prey fights as the game has it");
        return false;
    }
    bool ok = MH_CreateHook(thunk, reinterpret_cast<void*>(&EncounterDetour), reinterpret_cast<void**>(&g_encounter)) == MH_OK &&
              MH_EnableHook(thunk) == MH_OK;
    logx::Line(L"[prey] OnEncounterOverlap's thunk at %p%s", thunk, ok ? L" hooked" : L" - could not hook");
    if (!ok) g_encounter = nullptr;
    return ok;
}

}  // namespace

namespace prey {

void Peace(const std::wstring& types) {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_peaceMutex);
        g_peace.clear();
        size_t start = 0;
        while (start < types.size()) {
            size_t end = types.find(L',', start);
            std::wstring t = types.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!t.empty()) g_peace.insert(_wtoi(t.c_str()));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        n = g_peace.size();
    }
    if (n > 0) HookEncounter();
    logx::Line(L"[prey] %zu prey kinds start no fight when touched", n);
}


bool HookFrom(void* preyObject) {
    if (g_tried || !preyObject) return g_react != nullptr;
    g_tried = true;
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Wrc.WrcPouncePrey:OnCharacterDetected"));
    auto* thunk = fn ? reinterpret_cast<uint8_t*>(fn->GetFunc()) : nullptr;
    if (!thunk || !scan::BodyContains(thunk, "FF 90 F0 09 00 00", 1)) {
        logx::Line(L"[prey] OnCharacterDetected's thunk %p doesn't call the reaction slot - anchors disagree, prey flee OFF", thunk);
        return false;
    }
    auto* target = (*reinterpret_cast<uint8_t***>(preyObject))[kReactSlot / 8];
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&ReactDetour), reinterpret_cast<void**>(&g_react)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    logx::Line(L"[prey] detection reaction at %p (confirmed: OnCharacterDetected's thunk calls its slot)%s", target,
               ok ? L"" : L" - could not hook");
    if (!ok) g_react = nullptr;
    return ok;
}

void Configure(bool on, const std::wstring& excludedTags) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_excluded.clear();
        size_t start = 0;
        while (start <= excludedTags.size()) {
            size_t end = excludedTags.find(L',', start);
            std::wstring t = excludedTags.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            while (!t.empty() && t.front() == L' ') t.erase(t.begin());
            while (!t.empty() && t.back() == L' ') t.pop_back();
            if (!t.empty()) g_excluded.push_back(t);
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    if (g_on.exchange(on) != on || on)
        logx::Line(L"[prey] prey flee %s (%zu size tags keep the game's reaction)", on ? L"on" : L"off", g_excluded.size());
}

}  // namespace prey
