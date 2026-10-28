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

}  // namespace

namespace prey {

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
