// Hunting by chance (tickets.md 9, operator 2026-10-27): the game's prey instakill always works; with HuntRoll on, the
// moment the cat LANDS on the prey is rolled - chance = the cat's base chance (by rank, set from Lua) x the prey's
// factor (by its CharacterType, e.g. rabbit 0.5), never above 100% - critical miss. Only the CharacterTypes Lua lists
// are rolled: NPC cats carry the same component for the game's sneak attack, which stays as it is. A miss skips the landing (no
// hunted sequence - the prey carries on as the game makes it); a hit runs it. Both are Lua events: HuntCaught /
// HuntMissed (prey full name, cat full name, CharacterType value, chance %).
// The landing: USwtAdventurePreyComponent's UFunction OnHunterLanded(Hit, Character) is native; its exec thunk calls
// the component's VIRTUAL vtable +0xAA0 (rcx = component, rdx = &Hit, r8 = the cat) - hooked from a live component's
// vtable. Anchor: the thunk (from the UFunction by name) calls exactly that slot (call qword ptr [rax+0AA0h]);
// tools/check_hooks.py checks the same offline.
// EXP for a catch (operator 2026-10-27, what the fight would have given): the level component's resource "add value"
// is its VIRTUAL vtable +0x508 (rcx = component, xmm1 = amount): it scales the amount by the EXP multiplier
// (+0x114) and checks the current value (+0x10C) against the next level. Anchors: that function's body reads both
// fields exactly once (movss xmm1, [rbx+10Ch] / mulss xmm6, [rbx+114h]).
// The amounts: the game's NonAsset CSV CombatBaseExperience, loaded by the tuning manager (UWrcTuningManager, on the
// game instance) into an unreflected TMap<FString, FSwtAdventureCombatBaseExperience> at +0x460 - elements of 0x28:
// key FString, the row (vptr, BaseExperience, DifficultyOffset), hash links. Anchors: the manager's init (vtable
// +0x378) tail-calls the loader that addresses +0x460 (lea rdx, [rbx+460h]); the map must have the shape (every key
// a sane string, every row the same row type).
#include "hunt.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
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

using LandedFn = void(__fastcall*)(void* component, void* hit, void* character);
LandedFn g_landed = nullptr;
bool g_tried = false;
constexpr int kLandedSlot = 0xAA0;

std::atomic<bool> g_on{false};
std::mutex g_mutex;
double g_base = 1.0;                       // the cat's chance before the prey's factor (1.0 = 100%)
double g_critMiss = 0.05;
std::map<int, double> g_factors;           // CharacterType value -> factor (only these kinds are rolled)

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Field(const std::wstring& s, int index, wchar_t sep) {
    size_t start = 0;
    for (int i = 0; i < index; ++i) {
        start = s.find(sep, start);
        if (start == std::wstring::npos) return L"";
        ++start;
    }
    size_t end = s.find(sep, start);
    return s.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

double Roll() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// THE ROLL, ONCE PER POUNCE (operator 2026-10-30: a failed roll makes the prey flee AS THE CAT JUMPS): decided at the
// jump (OnHuntedPounceReached - the walk is over, the leap starts) - a miss goes to Lua at once (HuntMissed: the prey
// runs off) and the landing then catches nothing; a catch goes at the landing (HuntCaught). A prey Lua marked as
// spotted during the walk (HuntFail) is a miss. A landing without a jump decision rolls there (as before).
struct Decision { bool caught; double chance; bool announced; };
std::map<void*, Decision> g_decisions;      // prey -> this pounce's result (cleared at the landing / the sequence end)

// the roll for a prey of a rolled kind (true = there is a decision now)
bool Decide(UObject* prey, const char* when) {
    auto* type = prey->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CharacterType"));
    int typeValue = type ? *type : -1;
    double chance;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_decisions.count(prey)) return true;
        auto it = g_factors.find(typeValue);
        if (it == g_factors.end()) return false;
        chance = g_base * it->second;
        if (chance > 1.0 - g_critMiss) chance = 1.0 - g_critMiss;
    }
    double r = Roll();
    bool caught = r < chance;
    logx::Line(L"[hunt] t=%lld %S on %s (type %d): chance %.0f%%, rolled %.0f -> %s", NowMs(), when, prey->GetName().c_str(),
               typeValue, chance * 100.0, r * 100.0, caught ? L"CAUGHT" : L"missed");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_decisions[prey] = {caught, chance, false};
    return true;
}

void Announce(UObject* prey, UObject* cat, bool caught, double chance) {
    auto* type = prey->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CharacterType"));
    char pct[16];
    snprintf(pct, sizeof(pct), "%.0f", chance * 100.0);
    bridge::Emit(caught ? "HuntCaught" : "HuntMissed",
                 {Narrow(prey->GetFullName()), cat ? Narrow(cat->GetFullName()) : std::string(), std::to_string(type ? *type : -1), pct});
}

void __fastcall LandedDetour(void* componentPtr, void* hit, void* characterPtr) {
    auto* component = static_cast<UObject*>(componentPtr);
    auto* cat = static_cast<UObject*>(characterPtr);
    // a component's outer is its actor (the prey)
    UObject* prey = component ? component->GetOuterPrivate() : nullptr;
    if (!g_on || !prey || !cat || !Decide(prey, "landed")) {
        g_landed(componentPtr, hit, characterPtr);
        return;
    }
    Decision d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = g_decisions[prey];
        g_decisions.erase(prey);
    }
    logx::Line(L"[hunt] t=%lld landed on %s -> %s", NowMs(), prey->GetName().c_str(), d.caught ? L"caught" : L"nothing to catch");
    if (!d.announced) Announce(prey, cat, d.caught, d.chance);
    if (d.caught) g_landed(componentPtr, hit, characterPtr);
}

// THE POUNCE'S TIMELINE (2026-10-30, the pounce walk spec): the prey component's other two hunt slots, logged with
// the time and sent to Lua - +0xAB0 OnHuntedPounceReached(bStarted, requester) -> HuntPounce (prey, "1" / "0"),
// +0xAA8 OnHuntedSequencerFinish(sequenceId) -> HuntSequenceEnd (prey). Anchors: each UFunction's thunk calls its slot.
using PounceReachedFn = void(__fastcall*)(void* component, bool started, void* requester);
using SequenceFinishFn = void(__fastcall*)(void* component, uint64_t sequenceTag);
PounceReachedFn g_pounceReached = nullptr;
SequenceFinishFn g_sequenceFinish = nullptr;
constexpr int kPounceReachedSlot = 0xAB0, kSequenceFinishSlot = 0xAA8;

void __fastcall PounceReachedDetour(void* componentPtr, bool started, void* requester) {
    auto* component = static_cast<UObject*>(componentPtr);
    UObject* prey = component ? component->GetOuterPrivate() : nullptr;
    logx::Line(L"[hunt] t=%lld pounce reached (started %d) on %s by %s", NowMs(), started ? 1 : 0,
               prey ? prey->GetName().c_str() : L"?", requester ? static_cast<UObject*>(requester)->GetName().c_str() : L"-");
    if (prey) bridge::Emit("HuntPounce", {Narrow(prey->GetFullName()), started ? "1" : "0"});
    // the jump: the roll now - a miss is told at once (the prey bolts as the cat leaps)
    if (g_on && prey && !started && Decide(prey, "jumped")) {
        bool tell = false;
        double chance = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Decision& d = g_decisions[prey];
            if (!d.caught && !d.announced) {
                d.announced = true;
                tell = true;
                chance = d.chance;
            }
        }
        // (outside the lock: Lua's handlers run inside the emit and may send HuntFail)
        if (tell) Announce(prey, static_cast<UObject*>(requester), false, chance);
    }
    g_pounceReached(componentPtr, started, requester);
}

void __fastcall SequenceFinishDetour(void* componentPtr, uint64_t tag) {
    auto* component = static_cast<UObject*>(componentPtr);
    UObject* prey = component ? component->GetOuterPrivate() : nullptr;
    logx::Line(L"[hunt] t=%lld hunted sequence finished on %s", NowMs(), prey ? prey->GetName().c_str() : L"?");
    if (prey) bridge::Emit("HuntSequenceEnd", {Narrow(prey->GetFullName())});
    if (prey) { std::lock_guard<std::mutex> lock(g_mutex); g_decisions.erase(prey); }
    g_sequenceFinish(componentPtr, tag);
}

bool HookSlot(void* preyComponent, const wchar_t* ufunction, const char* callPattern, int slot, void* detour, void** original,
              const wchar_t* what) {
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, ufunction);
    auto* thunk = fn ? reinterpret_cast<uint8_t*>(fn->GetFunc()) : nullptr;
    if (!thunk || !scan::BodyContains(thunk, callPattern, 1)) {
        logx::Line(L"[hunt] %s: its thunk %p doesn't call slot +0x%X - not hooked", what, thunk, slot);
        return false;
    }
    auto* target = (*reinterpret_cast<uint8_t***>(preyComponent))[slot / 8];
    bool ok = MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    logx::Line(L"[hunt] %s at %p%s", what, target, ok ? L"" : L" - could not hook");
    if (!ok) *original = nullptr;
    return ok;
}

using AddValueFn = void(__fastcall*)(void* component, float amount);
constexpr int kAddValueSlot = 0x508;
constexpr int kTuningInitSlot = 0x378;
constexpr size_t kExpMap = 0x460;

struct ExpElement {
    wchar_t* Key;
    int32_t KeyNum;
    int32_t KeyMax;
    void* RowVtable;
    int32_t BaseExperience;
    int32_t DifficultyOffset;
    int32_t HashNext;
    int32_t HashIndex;
};
static_assert(sizeof(ExpElement) == 0x28, "the map's element");

}  // namespace

namespace hunt {

// (no "heard on loud ground" at the landing any more: the landing spot is the prey's own - it is already caught then;
// prey hear the cat the game's way before the pounce, operator 2026-10-30. The command stays a no-op for old Lua.)
void SetLoud(bool, double) {}

// HuntFail|<prey full name>: this pounce on that prey fails (Lua: the prey spotted the cat during the walk and ran)
void Fail(const std::wstring& preyName) {
    std::wstring path = preyName;
    size_t sp = path.find(L' ');
    if (sp != std::wstring::npos) path = path.substr(sp + 1);
    auto* prey = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
    if (!prey) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_decisions[prey] = {false, 0.0, true};
    logx::Line(L"[hunt] t=%lld %s spotted the cat during the pounce - it fails", NowMs(), prey->GetName().c_str());
}

int CombatExperience(void* tuningManager, const std::wstring& kind) {
    if (!tuningManager) return -1;
    static void* anchoredFor = nullptr;
    if (anchoredFor != *reinterpret_cast<void**>(tuningManager)) {
        auto* init = (*reinterpret_cast<uint8_t***>(tuningManager))[kTuningInitSlot / 8];
        if (!scan::CalleeWhere(init, "48 8D 93 60 04 00 00", true)) {
            logx::Line(L"[hunt] the tuning manager's init %p doesn't load +0x%zX - anchors disagree, no combat EXP table", init, kExpMap);
            return -1;
        }
        anchoredFor = *reinterpret_cast<void**>(tuningManager);
    }
    struct Array {
        ExpElement* Data;
        int32_t Num;
        int32_t Max;
    };
    auto* arr = reinterpret_cast<Array*>(static_cast<uint8_t*>(tuningManager) + kExpMap);
    if (!arr->Data || arr->Num <= 0 || arr->Num > 1000 || arr->Max < arr->Num || IsBadReadPtr(arr->Data, sizeof(ExpElement) * arr->Num)) {
        logx::Line(L"[hunt] the combat EXP table isn't loaded (or not where it was)");
        return -1;
    }
    void* rowType = arr->Data[0].RowVtable;
    for (int32_t i = 0; i < arr->Num; ++i) {
        const ExpElement& e = arr->Data[i];
        if (e.RowVtable != rowType || !e.Key || e.KeyNum <= 0 || e.KeyNum > 256 || IsBadReadPtr(e.Key, e.KeyNum * 2)) {
            logx::Line(L"[hunt] the combat EXP table's element %d isn't a row - anchors disagree", i);
            return -1;
        }
        if (_wcsicmp(e.Key, kind.c_str()) == 0) return e.BaseExperience;
    }
    return -1;
}

bool AddResource(void* component, float amount, float* now) {
    if (!component) return false;
    // every quest resource (level / EXP, the fresh-kill pile, health, stamina: USwtQuestResourceComponent) has the
    // add-value at the same slot; each class's function there is checked once: it IS the anchored base add (reads the
    // value +0x10C, scales by +0x114) or calls it directly (an override)
    auto* fn = (*reinterpret_cast<uint8_t***>(component))[kAddValueSlot / 8];
    static std::mutex checkedMutex;
    static std::map<uint8_t*, bool> checked;
    {
        std::lock_guard<std::mutex> lock(checkedMutex);
        auto it = checked.find(fn);
        if (it == checked.end()) {
            auto anchored = [](uint8_t* f) {
                return f && scan::BodyContains(f, "F3 0F 10 8B 0C 01 00 00", 1) && scan::BodyContains(f, "F3 0F 59 B3 14 01 00 00", 1);
            };
            // the base add itself (the level component's slot), or a class's override around it (the fresh-kill pile's:
            // milestone before / base add / milestone after - VM 2026-10-28) - one direct call into the anchored base
            bool ok = anchored(fn) || anchored(scan::CalleeWhere(fn, "F3 0F 10 8B 0C 01 00 00"));
            it = checked.emplace(fn, ok).first;
            if (!ok) {
                auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
                logx::Line(L"[hunt] a resource's slot +0x%X (%p, exe VA 0x%llX) isn't the add-value - anchors disagree",
                           kAddValueSlot, fn, static_cast<unsigned long long>(fn - base) + 0x140000000ull);
            }
        }
        if (!it->second) return false;
    }
    reinterpret_cast<AddValueFn>(fn)(component, amount);
    if (now) *now = *reinterpret_cast<float*>(static_cast<uint8_t*>(component) + 0x10C);
    return true;
}

bool AddExperience(void* levelComponent, float amount) {
    if (!AddResource(levelComponent, amount, nullptr)) return false;
    logx::Line(L"[hunt] +%.0f EXP", amount);
    return true;
}

bool HookFrom(void* preyComponent) {
    if (g_tried || !preyComponent) return g_landed != nullptr;
    g_tried = true;
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                            STR("/Script/SwtAdventure.SwtAdventurePreyComponent:OnHunterLanded"));
    auto* thunk = fn ? reinterpret_cast<uint8_t*>(fn->GetFunc()) : nullptr;
    if (!thunk || !scan::BodyContains(thunk, "FF 90 A0 0A 00 00", 1)) {
        logx::Line(L"[hunt] OnHunterLanded's thunk %p doesn't call the landing slot - anchors disagree, hunting by chance OFF", thunk);
        return false;
    }
    auto* target = (*reinterpret_cast<uint8_t***>(preyComponent))[kLandedSlot / 8];
    bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&LandedDetour), reinterpret_cast<void**>(&g_landed)) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    logx::Line(L"[hunt] prey landing at %p (confirmed: OnHunterLanded's thunk calls its slot)%s", target,
               ok ? L"" : L" - could not hook");
    if (!ok) g_landed = nullptr;
    HookSlot(preyComponent, STR("/Script/SwtAdventure.SwtAdventurePreyComponent:OnHuntedPounceReached"), "FF 90 B0 0A 00 00",
             kPounceReachedSlot, reinterpret_cast<void*>(&PounceReachedDetour), reinterpret_cast<void**>(&g_pounceReached),
             L"pounce reached");
    HookSlot(preyComponent, STR("/Script/SwtAdventure.SwtAdventurePreyComponent:OnHuntedSequencerFinish"), "FF 90 A8 0A 00 00",
             kSequenceFinishSlot, reinterpret_cast<void*>(&SequenceFinishDetour), reinterpret_cast<void**>(&g_sequenceFinish),
             L"hunted sequence finish");
    return ok;
}

void Configure(const std::wstring& command) {
    bool on = Field(command, 1, L'|') == L"1";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_base = _wtof(Field(command, 2, L'|').c_str()) / 100.0;
        g_critMiss = _wtof(Field(command, 3, L'|').c_str()) / 100.0;
        g_factors.clear();
        std::wstring list = Field(command, 4, L'|');
        for (int i = 0;; ++i) {
            std::wstring pair = Field(list, i, L',');
            if (pair.empty()) break;
            size_t eq = pair.find(L'=');
            if (eq != std::wstring::npos) g_factors[_wtoi(pair.substr(0, eq).c_str())] = _wtof(pair.substr(eq + 1).c_str());
        }
    }
    g_on = on;
    logx::Line(L"[hunt] hunting by chance %s: base %.0f%%, critical miss %.0f%%, %zu prey factors", on ? L"on" : L"off",
               g_base * 100.0, g_critMiss * 100.0, g_factors.size());
}

}  // namespace hunt
