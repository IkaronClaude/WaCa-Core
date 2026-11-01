// The event bus: game events reach the Lua mods as calls, never as something Lua has to poll for.
//
// Source of events: UE4SS's ProcessEvent pre / post callbacks (its own hook - no detour of ours). Every UFunction that
// runs through ProcessEvent can become an event, including Blueprint events that Lua's RegisterHook can't reach: a
// widget blueprint's Construct / Destruct (the moment a screen enters / leaves the viewport) is a BlueprintImplementable
// event whose implementation lives in each WBP_*_C class, so there is no one function to hook - but every call
// passes ProcessEvent with the function's NAME. A watch is (function name, class, event name, pre/post): when a
// function of that name runs on an object of that class, the event fires with the object.
// Native code adds its own events (Emit): the Steam keyboard's answer, for one.
//
// Lua side, registered as globals in EVERY Lua mod's state (WaCa.lua wraps them as WaCa.On / WaCa.Watch):
//   WaCaNative_Watch(functionName, classPath, eventName, "pre" | "post" [, paramName])
//     paramName: a parameter of that function, handed to the handler after the calling object - an object as itself
//     (e.g. an anim notify's MeshComp - the notify object is shared by every cat playing that animation), anything
//     simple as text: a name, a gameplay tag (its name - e.g. the sequencer manager's sequenceId), a string, a
//     number, a bool ("true" / "false"), an enum (its number)
//   WaCaNative_Ready()            the mod's main.lua finished - events may be delivered from now on; the first thing
//                                 delivered is "Started" (on the game thread), so the mod can catch up on what is
//                                 already there (WaCa.lua replays ScreenShown for the screens already up)
//   WaCaNative_Command(text)      a native command (commands.cpp) -> true / false
// and each event calls the mod's global WaCaNative_OnEvent(eventName, object or nil, string args...).
//
// Threads: events are delivered on the GAME THREAD only, on a Lua thread of the mod's own state that UE4SS knows
// (so calls on the objects work). A mod's main.lua runs on UE4SS's loader thread while the game thread is already
// running, so nothing is delivered before the mod says it is ready. Mods using the event bus must not run Lua off
// the game thread (LoopAsync / ExecuteAsync use another OS thread on the same state: that is what corrupted the VM in
// 0.5.3).
#include "bridge.h"
#include "keys.h"

#include <Mod/LuaMod.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <LuaType/LuaUObject.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include "MinHook.h"
#include "appearance.h"
#include "prey.h"
#include "hunt.h"
#include "sequences.h"
#include "screens.h"
#include "fill.h"
#include "input.h"
#include "areas.h"
#include "combat.h"
#include "bonds.h"
#include "commands.h"
#include "tables.h"
#include "log.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

struct ModState {
    std::wstring name;
    LuaMod* mod = nullptr;                    // identifies the mod from any of its Lua threads (get_mod_ref)
    LuaMadeSimple::Lua* thread = nullptr;     // events run here; created by LuaMadeSimple, so UE4SS knows it
    bool ready = false;
    bool started = false;                     // "Started" delivered
};

struct Watch {
    FName function;
    std::wstring classPath;
    UClass* cls = nullptr;                    // resolved on first use (a class may load after the watch is made)
    bool post = true;
    std::string event;
    std::wstring param;                       // a parameter to deliver too ("" = none)
    FProperty* paramProp = nullptr;           // resolved with the function
    ModState* state = nullptr;
};

std::vector<std::unique_ptr<ModState>> g_states;
// the mods' states: added / removed on UE4SS's loader thread (a mod starting, a hot reload stopping it) while the game
// thread delivers into them - a removal under a delivery freed the state it ran in. Held while delivering (recursive:
// a handler's calls raise more events) and while adding / removing.
std::recursive_timed_mutex g_statesMutex;
std::vector<Watch> g_watches;
// mods register / drop watches on UE4SS's mod thread (start-up, reloads) while the game thread dispatches - the list is
// only touched under this lock, and events are delivered after it's released (a handler may add watches). A crash at
// start-up (VM 2026-10-27: main.dll reading freed memory in this loop) came from that race.
std::mutex g_watchesMutex;
std::atomic<size_t> g_watchCount{0};     // g_watches.size(), for the lock-free "nothing watched" fast path
std::atomic<bool> g_haveGameThread{false};
std::atomic<bool> g_startPending{false};      // a mod became ready: deliver its "Started" on the game thread
// UE4SS starts the Lua mods one after another on its loader thread while the game thread runs: a mod that is ready
// already got events (its Lua running on the game thread) while the loader thread started the next mods - both in
// UE4SS's shared machinery at once (hooks, object wrappers). VM 2026-10-31 / 11-01: twice the game died at start-up,
// a memcpy writing out of bounds under Deliver("Started") while 'WaCaGraphics ... starting mod'. So nothing is
// delivered while a mod is LOADING: from its on_lua_start until its main.lua says Ready (a mod that never does - not a
// WaCa mod - counts kMaxLoad from its start), and kLoadQuiet after the latest start. Started replays what was missed.
// (VM 2026-11-01: a 3 s window alone was too short - Clan Life's main.lua ran 12 s on a hot reload and the game hung.)
std::atomic<int64_t> g_lastLuaStart{0};       // steady-clock ms of the latest on_lua_start (any mod)
constexpr int64_t kLoadQuiet = 3000;
constexpr int64_t kMaxLoad = 30000;
std::mutex g_loadingMutex;
std::map<std::wstring, int64_t> g_loadingMods;   // mod name -> its on_lua_start (ms), until its Ready
std::atomic<bool> g_loadingCached{true};
std::atomic<int64_t> g_loadingCheckedAt{0};
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// (every ProcessEvent asks: the answer is re-worked at most every 250 ms, an atomic in between)
bool ModsLoading() {
    const int64_t now = NowMs();
    if (now - g_lastLuaStart.load(std::memory_order_acquire) < kLoadQuiet) return true;
    if (now - g_loadingCheckedAt.load(std::memory_order_relaxed) < 250) return g_loadingCached.load(std::memory_order_relaxed);
    bool loading = false;
    {
        std::lock_guard<std::mutex> lock(g_loadingMutex);
        for (auto& [name, startedAt] : g_loadingMods)
            if (now - startedAt < kMaxLoad) { loading = true; break; }
    }
    g_loadingCached.store(loading, std::memory_order_relaxed);
    g_loadingCheckedAt.store(now, std::memory_order_relaxed);
    return loading;
}
std::thread::id g_gameThread{};
int g_depth = 0;                              // nested events (a handler whose calls fire more events)
constexpr int kMaxDepth = 8;

std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Wide(const char* s) { return Wide(std::string_view(s ? s : "")); }

bool OnGameThread() {
    return g_haveGameThread.load(std::memory_order_acquire) && std::this_thread::get_id() == g_gameThread;
}

ModState* StateOf(const LuaMadeSimple::Lua& lua) {
    std::lock_guard<std::recursive_timed_mutex> lock(g_statesMutex);
    LuaMod* mod = get_mod_ref(lua);
    for (auto& s : g_states)
        if (s->mod == mod) return s.get();
    return nullptr;
}

std::string Narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, ' ');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// a watched parameter's value for Lua: an object, or text (kind 2), or nothing (0)
struct ParamValue {
    int kind = 0;
    UObject* object = nullptr;
    std::string text;
};

ParamValue ReadParam(FProperty* p, void* params) {
    ParamValue v;
    if (!p || !params) return v;
    uint8_t* at = static_cast<uint8_t*>(params) + p->GetOffset_Internal();
    if (CastField<FObjectProperty>(p)) {
        v.kind = 1;
        v.object = *reinterpret_cast<UObject**>(at);
    } else if (CastField<FNameProperty>(p)) {
        v.kind = 2;
        v.text = Narrow(reinterpret_cast<FName*>(at)->ToString());
    } else if (auto* sp = CastField<FStructProperty>(p)) {
        // a gameplay tag: its one FName (TagName) first
        if (sp->GetStruct() && sp->GetStruct()->GetName() == STR("GameplayTag")) {
            v.kind = 2;
            v.text = Narrow(reinterpret_cast<FName*>(at)->ToString());
        }
    } else if (CastField<FStrProperty>(p)) {
        v.kind = 2;
        v.text = Narrow(**reinterpret_cast<FString*>(at));
    } else if (auto* bp = CastField<FBoolProperty>(p)) {
        v.kind = 2;
        v.text = bp->GetPropertyValue(at) ? "true" : "false";
    } else if (auto* ep = CastField<FEnumProperty>(p)) {
        if (auto* under = ep->GetUnderlyingProperty()) {
            v.kind = 2;
            v.text = std::to_string(under->GetSignedIntPropertyValue(at));
        }
    } else if (auto* np = CastField<FNumericProperty>(p)) {
        v.kind = 2;
        v.text = np->IsFloatingPoint() ? std::to_string(np->GetFloatingPointPropertyValue(at))
                                       : std::to_string(np->GetSignedIntPropertyValue(at));
    }
    return v;
}

void Deliver(ModState& s, const std::string& event, UObject* object, std::initializer_list<std::string> args,
             const ParamValue* extra = nullptr) {
    if (!s.ready || !s.thread || ModsLoading()) return;
    const LuaMadeSimple::Lua& lua = *s.thread;
    if (!lua.is_global_function("WaCaNative_OnEvent")) return;
    if (g_depth >= kMaxDepth) {
        logx::Line(L"[bridge] %s: event %s dropped (nested %d deep)", s.name.c_str(), Wide(event).c_str(), g_depth);
        return;
    }
    ++g_depth;
    try {
        lua.prepare_function_call("WaCaNative_OnEvent");
        lua.set_string(event);
        if (object) LuaType::auto_construct_object(lua, object);
        else lua.set_nil();
        if (extra) {
            if (extra->kind == 1 && extra->object) LuaType::auto_construct_object(lua, extra->object);
            else if (extra->kind == 2) lua.set_string(extra->text);
            else lua.set_nil();
        }
        for (const auto& a : args) lua.set_string(a);
        lua.call_function(2 + (extra ? 1 : 0) + static_cast<int32_t>(args.size()), 0);
    } catch (const std::exception& e) {
        // the traceback is already in UE4SS.log (LuaMadeSimple's error handler)
        logx::Line(L"[bridge] %s: event %s failed: %s", s.name.c_str(), Wide(event).c_str(), Wide(e.what()).c_str());
    }
    --g_depth;
}

void DeliverStarted() {
    std::lock_guard<std::recursive_timed_mutex> lock(g_statesMutex);
    g_startPending.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < g_states.size(); ++i) {
        ModState& s = *g_states[i];
        if (!s.ready || s.started) continue;
        s.started = true;
        Deliver(s, "Started", nullptr, {});
    }
}

// DEV TRACE (2026-10-30, the pounce on the VM): WaCaNative_DevTrace("<class path>,<class path>", seconds) logs every
// UFunction that runs through ProcessEvent on an object of those classes - each (object, function) once per second -
// to see what the game does (and doesn't) around an action.
std::mutex g_traceMutex;
std::vector<std::wstring> g_tracePaths;
std::vector<UClass*> g_traceClasses;
std::chrono::steady_clock::time_point g_traceUntil{};
std::map<std::wstring, std::chrono::steady_clock::time_point> g_traceSeen;

void TraceCall(UObject* context, UFunction* function) {
    auto now = std::chrono::steady_clock::now();
    if (now > g_traceUntil) return;
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (g_traceClasses.size() != g_tracePaths.size()) {
        g_traceClasses.clear();
        for (auto& path : g_tracePaths)
            g_traceClasses.push_back(UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path.c_str()));
    }
    bool match = false;
    for (auto* c : g_traceClasses) if (c && context->IsA(c)) { match = true; break; }
    if (!match) return;
    std::wstring key = context->GetName() + L"::" + function->GetName();
    auto it = g_traceSeen.find(key);
    if (it != g_traceSeen.end() && now - it->second < std::chrono::seconds(1)) return;
    g_traceSeen[key] = now;
    logx::Line(L"[trace] %s", key.c_str());
}

void OnProcessEvent(bool post, UObject* context, UFunction* function, void* params) {
    if (!OnGameThread()) return;
    if (!post && context && function && std::chrono::steady_clock::now() <= g_traceUntil) TraceCall(context, function);
    if (ModsLoading()) return;                                 // (the loader thread is still starting mods)
    std::lock_guard<std::recursive_timed_mutex> statesLock(g_statesMutex);
    if (g_startPending.load(std::memory_order_relaxed)) DeliverStarted();
    if (g_watchCount.load(std::memory_order_relaxed) == 0 || !context || !function) return;
    const FName name = function->GetNamePrivate();
    struct Due { ModState* state; std::string event; ParamValue extra; bool hasExtra; };
    std::vector<Due> due;
    {
        std::lock_guard<std::mutex> lock(g_watchesMutex);
        for (auto& w : g_watches) {
            if (w.post != post || !(w.function == name)) continue;
            if (!w.cls) {
                w.cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, w.classPath.c_str());
                if (!w.cls) continue;
            }
            if (!context->IsA(w.cls)) continue;
            if (w.param.empty()) {
                due.push_back({w.state, w.event, {}, false});
                continue;
            }
            if (!w.paramProp) {
                w.paramProp = function->FindProperty(FName(w.param.c_str(), FNAME_Find));
                if (!w.paramProp) continue;
            }
            due.push_back({w.state, w.event, ReadParam(w.paramProp, params), true});
        }
    }
    for (auto& d : due) Deliver(*d.state, d.event, context, {}, d.hasExtra ? &d.extra : nullptr);
}

// --- Lua functions (the calling Lua state is the argument; arguments are consumed from index 1) ---

int LuaDevTrace(const LuaMadeSimple::Lua& lua) {
    if (!lua.is_string()) lua.throw_error("WaCaNative_DevTrace(\"<class path>,<class path>\", seconds)");
    std::wstring list = Wide(std::string(lua.get_string()));
    double seconds = lua.is_number() ? lua.get_number() : 10.0;
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_tracePaths.clear();
    g_traceClasses.clear();
    g_traceSeen.clear();
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L',', start);
        std::wstring p = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!p.empty() && p.front() == L' ') p.erase(p.begin());
        if (!p.empty()) g_tracePaths.push_back(p);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    g_traceUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
    logx::Line(L"[trace] %zu classes for %.0f s", g_tracePaths.size(), seconds);
    return 0;
}

int LuaWatch(const LuaMadeSimple::Lua& lua) {
    ModState* state = StateOf(lua);
    if (!state) return 0;
    if (!lua.is_string()) lua.throw_error("WaCaNative_Watch(functionName, classPath, eventName, \"pre\" | \"post\")");
    std::wstring function = Wide(std::string(lua.get_string()));
    std::wstring classPath = Wide(std::string(lua.get_string()));
    std::string event(lua.get_string());
    bool post = true;
    if (lua.is_string()) post = lua.get_string() != "pre";
    std::wstring param;
    if (lua.is_string()) param = Wide(std::string(lua.get_string()));
    Watch w;
    w.param = param;
    w.function = FName(function.c_str(), FNAME_Add);
    w.classPath = classPath;
    w.post = post;
    w.event = event;
    w.state = state;
    {
        std::lock_guard<std::mutex> lock(g_watchesMutex);
        g_watches.push_back(std::move(w));
        g_watchCount = g_watches.size();
    }
    logx::Line(L"[bridge] %s: %s on %s (%s) -> %s", state->name.c_str(), function.c_str(), classPath.c_str(),
               post ? L"post" : L"pre", Wide(event).c_str());
    return 0;
}

// CROSS-MOD EVENTS (operator 2026-10-28: the parts of WaCa are separate UE4SS mods = separate Lua states):
// WaCaNative_Broadcast(event, payload) hands an event to EVERY OTHER mod's Lua state as
// WaCaNative_OnEvent(event, nil, payload, fromMod) - the payload a JSON text (WaCa.lua encodes / decodes it; UObjects
// don't cross states). The sender dispatches to itself in Lua. Game thread only (the states run there).
// sticky events: the latest payload per event, for a mod that subscribes after it was sent
// (WaCaNative_LastBroadcast(event) -> payload, sender | nil)
std::map<std::string, std::pair<std::string, std::string>> g_sticky;

int LuaBroadcast(const LuaMadeSimple::Lua& lua) {
    ModState* from = StateOf(lua);
    if (!lua.is_string()) lua.throw_error("WaCaNative_Broadcast(event, payloadJson [, sticky])");
    std::string event(lua.get_string());
    std::string payload = lua.is_string() ? std::string(lua.get_string()) : std::string();
    bool sticky = lua.is_bool() ? lua.get_bool() : false;
    std::string sender = from ? Narrow(from->name) : std::string("?");
    // (a sticky value is kept even when it can't be delivered yet: mods read it when they subscribe)
    if (sticky) g_sticky[event] = {payload, sender};
    if (!OnGameThread()) {
        logx::Line(L"[bridge] broadcast %s off the game thread - not delivered%s", Wide(event).c_str(),
                   sticky ? L" (kept as sticky)" : L"");
        return 0;
    }
    std::lock_guard<std::recursive_timed_mutex> statesLock(g_statesMutex);
    for (size_t i = 0; i < g_states.size(); ++i) {
        if (g_states[i].get() == from) continue;
        Deliver(*g_states[i], event, nullptr, {payload, sender});
    }
    return 0;
}

int LuaLastBroadcast(const LuaMadeSimple::Lua& lua) {
    if (!lua.is_string()) return 0;
    std::string event(lua.get_string());
    auto it = g_sticky.find(event);
    if (it == g_sticky.end()) return 0;
    lua.set_string(it->second.first);
    lua.set_string(it->second.second);
    return 2;
}

// WaCaNative_ModsLoading() -> true while Lua mods are still starting (the same answer the events wait on): WaCa.lua's
// timer guard holds UE4SS's game-thread callbacks (ExecuteInGameThread / ...WithDelay) till then - they raced the
// loader thread the same way (VM 2026-11-01: two start-up crashes 16 s in, a Lua callback reading 0x10)
int LuaModsLoading(const LuaMadeSimple::Lua& lua) {
    lua.set_bool(ModsLoading());
    return 1;
}

int LuaReady(const LuaMadeSimple::Lua& lua) {
    if (ModState* state = StateOf(lua)) {
        {
            std::lock_guard<std::mutex> lock(g_loadingMutex);
            g_loadingMods.erase(state->name);
        }
        g_loadingCheckedAt.store(0, std::memory_order_relaxed);     // (re-worked on the next event)
        state->ready = true;
        g_startPending.store(true, std::memory_order_release);
        logx::Line(L"[bridge] %s: ready for events", state->name.c_str());
    }
    return 0;
}

// dev: the bindings of a multicast delegate - WaCaNative_DevDelegate(object address, property offset) -> text
// "Object.Function; ..." (a TArray of FScriptDelegate = FWeakObjectPtr + FName, 16 bytes each on this build)
int LuaDevDelegate(const LuaMadeSimple::Lua& lua) {
    auto object = static_cast<uintptr_t>(lua.get_integer());
    auto offset = static_cast<uintptr_t>(lua.get_integer());
    struct Binding {
        FWeakObjectPtr Object;
        uint32_t NameIndex;
        uint32_t NameNumber;
    };
    struct Array {
        Binding* Data;
        int32_t Num;
        int32_t Max;
    };
    std::wstring out;
    auto* arr = reinterpret_cast<Array*>(object + offset);
    if (arr->Num < 0 || arr->Num > 64) {
        out = L"(not a delegate array)";
    } else if (arr->Num == 0) {
        out = L"(nothing bound)";
    }
    for (int32_t i = 0; arr->Num > 0 && arr->Num <= 64 && i < arr->Num; ++i) {
        UObject* bound = arr->Data[i].Object.Get();
        FName fn(arr->Data[i].NameIndex, arr->Data[i].NameNumber);
        if (!out.empty()) out += L"; ";
        out += bound ? bound->GetFullName() : std::wstring(L"(gone)");
        out += L" :: ";
        out += fn.ToString();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    lua.set_string(utf8);
    return 1;
}

// dev: an input component's axis bindings - WaCaNative_DevAxisBindings(component address) -> text "AxisName -> code
// addresses ..." per binding. AxisBindings is not reflected: the array is found by its shape (data, num, max) and its
// element size / name offset by EVERY element's FName resolving to a name (one an axis-like name: "Player_...",
// "Axis..."). Each binding is then searched three pointers deep (TSharedPtr -> TDelegate -> its heap instance -> the
// bound method) for addresses into the game's code: the handler the axis calls
std::wstring CodeOwner(uintptr_t addr) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto lo = reinterpret_cast<uintptr_t>(base);
    if (addr < lo || addr >= lo + nt->OptionalHeader.SizeOfImage) return L"";
    DWORD64 imageBase = 0;
    auto* fn = RtlLookupFunctionEntry(addr, &imageBase, nullptr);
    if (!fn) return L"";
    wchar_t buf[64];
    swprintf_s(buf, L"0x%llX (func 0x%llX)", static_cast<unsigned long long>(addr - lo + 0x140000000ull),
               static_cast<unsigned long long>(fn->BeginAddress + 0x140000000ull));
    return buf;
}

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

// code addresses reachable from `at` (n bytes) within `depth` pointer hops, with the path ("+8>+0>+10")
void CodeWithin(const uint8_t* at, int n, int depth, const std::wstring& path, std::wstring& out) {
    for (int q = 0; q + 8 <= n; q += 8) {
        auto v = *reinterpret_cast<const uintptr_t*>(at + q);
        wchar_t step[16];
        swprintf_s(step, L"+%X", q);
        std::wstring code = CodeOwner(v);
        if (!code.empty()) out += L" [" + path + step + L"]" + code;
        else if (depth > 0 && v > 0x10000 && Readable(reinterpret_cast<void*>(v), 0x30))
            CodeWithin(reinterpret_cast<const uint8_t*>(v), 0x30, depth - 1, path + step + L">", out);
    }
}

int LuaDevAxisBindings(const LuaMadeSimple::Lua& lua) {
    auto* comp = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lua.get_integer()));
    std::wstring out;
    for (int off = 0x28; comp && off < 0x400 && out.empty(); off += 8) {
        if (!Readable(comp + off, 16)) break;
        auto* data = *reinterpret_cast<uint8_t**>(comp + off);
        int32_t num = *reinterpret_cast<int32_t*>(comp + off + 8);
        int32_t max = *reinterpret_cast<int32_t*>(comp + off + 12);
        if (num < 1 || num > 128 || max < num || max > 1024 || !Readable(data, 16)) continue;
        for (int stride = 0x18; stride <= 0x100 && out.empty(); stride += 8) {
            if (!Readable(data, static_cast<size_t>(stride) * num)) break;
            for (int nameOff = 0; nameOff <= 8 && out.empty(); nameOff += 4) {
                auto nameAt = [&](int i) {
                    auto* p = reinterpret_cast<uint32_t*>(data + i * stride + nameOff);
                    if (p[0] == 0 || p[0] > 0x4000000 || p[1] > 64) return std::wstring();
                    return FName(p[0], p[1]).ToString();
                };
                bool all = true, axisLike = false;
                for (int i = 0; i < num && all; ++i) {
                    std::wstring name = nameAt(i);
                    all = !name.empty() && name != L"None";
                    axisLike = axisLike || name.rfind(L"Player_", 0) == 0 || name.rfind(L"Axis", 0) == 0 ||
                               name.find(L"Camera") != std::wstring::npos;
                }
                if (!all || !axisLike) continue;
                wchar_t head[96];
                swprintf_s(head, L"array at +0x%X, %d bindings, stride 0x%X, name at +%d\n", off, num, stride, nameOff);
                out = head;
                for (int i = 0; i < num; ++i) {
                    out += L"  " + nameAt(i) + L" ->";
                    CodeWithin(data + i * stride, stride, 3, L"", out);
                    // a Blueprint binding: FScriptDelegate { FWeakObjectPtr (index, serial), FName function } after
                    // the native delegate (TSharedPtr, 0x10) that follows the 0x10 FInputBinding head
                    if (stride >= 0x30) {
                        auto* fnName = reinterpret_cast<uint32_t*>(data + i * stride + 0x28);
                        auto* weak = reinterpret_cast<int32_t*>(data + i * stride + 0x20);
                        if (fnName[0] && fnName[0] < 0x4000000 && fnName[1] < 64) {
                            FUObjectItem* item = weak[0] > 0 && weak[0] < FUObjectArray::GetNumElements() ? FUObjectArray::IndexToObject(weak[0]) : nullptr;
                            UObject* obj = item ? item->GetUObject() : nullptr;
                            out += L" bp " + (obj ? obj->GetName() : std::wstring(L"?")) + L"." +
                                   FName(fnName[0], fnName[1]).ToString();
                        }
                    }
                    out += L"\n";
                }
            }
        }
    }
    if (out.empty()) out = L"(no axis binding array found)";
    logx::Line(L"[dev] axis bindings:\n%s", out.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    lua.set_string(utf8);
    return 1;
}

// dev: who reads / writes an axis value - WaCaNative_DevWatchAxis(component address, axis name). A hardware watchpoint
// (debug register 0, read or write, 4 bytes) on that binding's AxisValue in every thread; each instruction that touches
// it is logged once with its function, then the watch disarms itself after kWatchHits hits. For finding the code that
// consumes an axis (the camera) when nothing is bound to it but its value
constexpr int kWatchHits = 400;
std::atomic<int> g_watchHits{0};
std::mutex g_watchMutex;
std::vector<uintptr_t> g_watchSeen;
PVOID g_watchHandler = nullptr;
int g_watchLenBits = 3;                                   // DR7 length: 3 = 4 bytes, 2 = 8 bytes

void SetWatch(uintptr_t addr) {
    // from a helper thread: a thread can't reliably set its own debug registers, and the caller (the game thread) is
    // one of the threads to arm
    std::thread([addr] {
        DWORD self = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
            HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!t) continue;
            SuspendThread(t);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = addr;
                c.Dr7 = addr ? (c.Dr7 & ~0xF0003ull) | 1ull | (3ull << 16) | (static_cast<unsigned long long>(g_watchLenBits) << 18) : (c.Dr7 & ~0xF0003ull);
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
            CloseHandle(t);
        }
        CloseHandle(snap);
    }).join();
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    uintptr_t rip = e->ContextRecord->Rip;
    int hit = ++g_watchHits;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        if (std::find(g_watchSeen.begin(), g_watchSeen.end(), rip) == g_watchSeen.end()) {
            g_watchSeen.push_back(rip);
            first = true;
        }
    }
    if (first) {
        std::wstring where = CodeOwner(rip);
        if (where.empty()) {
            // another module: its name + the offset
            HMODULE m = nullptr;
            wchar_t path[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(rip), &m)) GetModuleFileNameW(m, path, MAX_PATH);
            wchar_t buf[MAX_PATH + 64];
            swprintf_s(buf, L"%s+0x%llX", wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path,
                       static_cast<unsigned long long>(rip - reinterpret_cast<uintptr_t>(m)));
            where = buf;
        }
        // a leaf accessor (GetAxisValue) stops on its ret: the caller - the actual consumer - is at [rsp]
        if (*reinterpret_cast<uint8_t*>(rip) == 0xC3)
            where += L", returning to " + CodeOwner(*reinterpret_cast<uintptr_t*>(e->ContextRecord->Rsp));
        // the callers (a write inside a generic helper - TArray growth - says little on its own): unwound from the
        // faulting context through the exe's unwind data
        CONTEXT c = *e->ContextRecord;
        for (int frame = 0; frame < 5; ++frame) {
            DWORD64 imageBase = 0;
            auto* fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fn) break;
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
            std::wstring caller = CodeOwner(c.Rip);
            if (caller.empty()) break;
            where += L"\n      called from " + caller;
        }
        logx::Line(L"[dev] watch: touched by the instruction before %s (thread %lu, hit %d)", where.c_str(), GetCurrentThreadId(), hit);
    }
    if (hit >= kWatchHits) {
        e->ContextRecord->Dr7 &= ~0xF0003ull;                     // this thread now; the others from a helper
        if (hit == kWatchHits) {
            logx::Line(L"[dev] watch: %d hits - disarmed", hit);
            std::thread([] { SetWatch(0); }).detach();
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

int LuaDevWatchAxis(const LuaMadeSimple::Lua& lua) {
    auto* comp = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(lua.get_integer()));
    std::string axis(lua.get_string());
    std::wstring want(axis.begin(), axis.end());
    // UInputComponent::AxisBindings at +0xE0 (data, num), elements of 0x38: the name at +4, AxisValue at +0x30
    auto* data = comp ? *reinterpret_cast<uint8_t**>(comp + 0xE0) : nullptr;
    int32_t num = comp ? *reinterpret_cast<int32_t*>(comp + 0xE8) : 0;
    uintptr_t target = 0;
    for (int i = 0; data && i < num && i < 128; ++i) {
        auto* p = reinterpret_cast<uint32_t*>(data + i * 0x38 + 4);
        if (FName(p[0], p[1]).ToString() == want) target = reinterpret_cast<uintptr_t>(data + i * 0x38 + 0x30);
    }
    if (!target) {
        logx::Line(L"[dev] watch: no axis binding %s", want.c_str());
        lua.set_bool(false);
        return 1;
    }
    if (!g_watchHandler) g_watchHandler = AddVectoredExceptionHandler(1, &WatchHandler);
    g_watchHits = 0;
    g_watchLenBits = 3;
    { std::lock_guard<std::mutex> lock(g_watchMutex); g_watchSeen.clear(); }
    SetWatch(target);
    logx::Line(L"[dev] watch: armed on %s's AxisValue at %p", want.c_str(), reinterpret_cast<void*>(target));
    lua.set_bool(true);
    return 1;
}

// dev: n floats at an address as text - WaCaNative_DevReadFloats(address, n) (unreflected fields, e.g. limits)
int LuaDevReadFloats(const LuaMadeSimple::Lua& lua) {
    auto* p = reinterpret_cast<const float*>(static_cast<uintptr_t>(lua.get_integer()));
    int n = static_cast<int>(lua.get_integer());
    std::string out;
    for (int i = 0; p && i < n && i < 64 && Readable(p + i, 4); ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%.3f", i ? " " : "", p[i]);
        out += buf;
    }
    lua.set_string(out);
    return 1;
}

// dev: where an object holds a multicast delegate bound to a function name - WaCaNative_DevFindBinding(object address,
// object size, function name) -> "+0xOFF (bound object), ..." (a TArray of {weak object, FName} by shape, every
// element's object alive)
int LuaDevFindBinding(const LuaMadeSimple::Lua& lua) {
    auto object = static_cast<uintptr_t>(lua.get_integer());
    auto size = static_cast<uintptr_t>(lua.get_integer());
    std::wstring want = Wide(std::string(lua.get_string()));
    struct Binding {
        FWeakObjectPtr Object;
        uint32_t NameIndex;
        uint32_t NameNumber;
    };
    struct Array {
        Binding* Data;
        int32_t Num;
        int32_t Max;
    };
    std::wstring out;
    for (uintptr_t off = 0x28; off + sizeof(Array) <= size; off += 8) {
        auto* arr = reinterpret_cast<Array*>(object + off);
        if (!Readable(arr, sizeof(Array)) || arr->Num <= 0 || arr->Num > 32 || arr->Max < arr->Num || arr->Max > 64) continue;
        if (!Readable(arr->Data, sizeof(Binding) * arr->Num)) continue;
        for (int32_t i = 0; i < arr->Num; ++i) {
            if (arr->Data[i].NameIndex == 0 || arr->Data[i].NameIndex > 0x4000000) continue;
            UObject* bound = arr->Data[i].Object.Get();
            if (!bound) continue;
            FName fn(arr->Data[i].NameIndex, arr->Data[i].NameNumber);
            if (fn.ToString() != want) continue;
            wchar_t buf[32];
            swprintf_s(buf, L"+0x%llX ", static_cast<unsigned long long>(off));
            if (!out.empty()) out += L", ";
            out += buf + bound->GetName();
        }
    }
    if (out.empty()) out = L"(none)";
    int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    lua.set_string(utf8);
    return 1;
}

// dev: a live object's vtable as an exe address (image base 0x140000000) - WaCaNative_DevVtable(object) -> integer
int LuaDevVtable(const LuaMadeSimple::Lua& lua) {
    auto object = static_cast<uintptr_t>(lua.get_integer());
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int64_t out = 0;
    if (object && Readable(reinterpret_cast<void*>(object), 8)) {
        auto vtable = *reinterpret_cast<uintptr_t*>(object);
        if (vtable > base) out = static_cast<int64_t>(vtable - base + 0x140000000ull);
    }
    lua.set_integer(out);
    return 1;
}

// dev: n int32s from an address - WaCaNative_DevReadInts(address, n) -> "v0 v1 ..." (unreflected fields)
int LuaDevReadInts(const LuaMadeSimple::Lua& lua) {
    auto* p = reinterpret_cast<const int32_t*>(static_cast<uintptr_t>(lua.get_integer()));
    int n = static_cast<int>(lua.get_integer());
    std::string out;
    for (int i = 0; p && i < n && i < 128 && Readable(p + i, 4); ++i) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%d", i ? " " : "", p[i]);
        out += buf;
    }
    lua.set_string(out);
    return 1;
}

// dev: a live object's virtual function - WaCaNative_DevVirtual(object address, vtable byte offset) -> its owner
// ("module+offset", as the watch stacks print it)
int LuaDevVirtual(const LuaMadeSimple::Lua& lua) {
    auto object = static_cast<uintptr_t>(lua.get_integer());
    auto offset = static_cast<uintptr_t>(lua.get_integer());
    std::wstring out = L"(unreadable)";
    if (object && Readable(reinterpret_cast<void*>(object), 8)) {
        auto vtable = *reinterpret_cast<uintptr_t*>(object);
        if (vtable && Readable(reinterpret_cast<void*>(vtable + offset), 8)) {
            out = CodeOwner(*reinterpret_cast<uintptr_t*>(vtable + offset));
        }
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    lua.set_string(utf8);
    return 1;
}

// dev: the same watchpoint on any address - WaCaNative_DevWatchAddress(address, 4 or 8 bytes)
int LuaDevWatchAddress(const LuaMadeSimple::Lua& lua) {
    auto target = static_cast<uintptr_t>(lua.get_integer());
    int bytes = static_cast<int>(lua.get_integer());
    if (!target || (bytes == 8 ? target % 8 : target % 4)) {
        logx::Line(L"[dev] watch: bad address %p (needs %d-byte alignment)", reinterpret_cast<void*>(target), bytes == 8 ? 8 : 4);
        lua.set_bool(false);
        return 1;
    }
    if (!g_watchHandler) g_watchHandler = AddVectoredExceptionHandler(1, &WatchHandler);
    g_watchHits = 0;
    g_watchLenBits = bytes == 8 ? 2 : 3;
    { std::lock_guard<std::mutex> lock(g_watchMutex); g_watchSeen.clear(); }
    SetWatch(target);
    logx::Line(L"[dev] watch: armed on %p (%d bytes)", reinterpret_cast<void*>(target), bytes == 8 ? 8 : 4);
    lua.set_bool(true);
    return 1;
}

// dev: who asks for a command's axis, and what they get - WaCaNative_DevTraceCommandAxis(exe offset of the command
// axis getter, calls to log). The getter (2026-10-25 build: 0x141828B30, float (this, const ECommands*, dead zone)) is
// the game's per-command axis read with the player's invert settings; this logs each distinct caller (return address)
// with the command, dead zone and result, for the next N calls. Dev only: the address is passed in, not scanned
using CommandAxisFn = float(__fastcall*)(void* self, const uint8_t* command, float deadZone);
CommandAxisFn g_commandAxis = nullptr;
std::atomic<int> g_commandAxisLeft{0};

float __fastcall CommandAxisDetour(void* self, const uint8_t* command, float deadZone) {
    float v = g_commandAxis(self, command, deadZone);
    if (g_commandAxisLeft > 0 && --g_commandAxisLeft >= 0) {
        auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        static std::vector<std::pair<uintptr_t, int>> seen;
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        int cmd = command ? *command : -1;
        if (std::find(seen.begin(), seen.end(), std::make_pair(ret, cmd)) == seen.end()) {
            seen.emplace_back(ret, cmd);
            logx::Line(L"[dev] command axis %d (dead zone %.3f) = %.3f, asked from %s, this %p", cmd, deadZone, v,
                       CodeOwner(ret).c_str(), self);
        }
    }
    return v;
}

int LuaDevTraceCommandAxis(const LuaMadeSimple::Lua& lua) {
    auto offset = static_cast<uintptr_t>(lua.get_integer());
    int calls = static_cast<int>(lua.get_integer());
    auto* target = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + offset;
    if (!g_commandAxis) {
        bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&CommandAxisDetour), reinterpret_cast<void**>(&g_commandAxis)) == MH_OK &&
                  MH_EnableHook(target) == MH_OK;
        logx::Line(L"[dev] command axis trace at %p: %s", target, ok ? L"hooked" : L"could not hook");
        if (!ok) { g_commandAxis = nullptr; lua.set_bool(false); return 1; }
    }
    g_commandAxisLeft = calls;
    lua.set_bool(true);
    return 1;
}

// WaCaNative_HookPrey(a live prey's address) -> true / false: hooks the prey detection reaction from its vtable
// (prey.cpp; once - later calls just report)
int LuaHookPrey(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    lua.set_bool(prey::HookFrom(reinterpret_cast<void*>(addr)));
    return 1;
}

// WaCaNative_HookHunt(a live USwtAdventurePreyComponent's address) -> true / false: hunting by chance (hunt.cpp)
int LuaHookHunt(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    lua.set_bool(hunt::HookFrom(reinterpret_cast<void*>(addr)));
    return 1;
}

// WaCaNative_HuntDetect(a BTService_DetectionTask's address, a detection component's address, type) -> true / false:
// one pass of the game's own prey detection on that component (hunt.cpp - sight during the pounce walk)
int LuaHuntDetect(const LuaMadeSimple::Lua& lua) {
    auto service = static_cast<uintptr_t>(lua.get_integer());
    auto component = static_cast<uintptr_t>(lua.get_integer());
    int type = static_cast<int>(lua.get_integer());
    lua.set_bool(hunt::Detect(reinterpret_cast<void*>(service), reinterpret_cast<void*>(component), type));
    return 1;
}

// WaCaNative_PlaySequence(sequencer manager address, tag, type, cast [, music event]) -> true / false: one of the game's own scenes
// (a rank-up ceremony: type 3, "Gameplay.Narrative.RankUp.<Clan>.<Rank>") played the game's way, with our cats in its
// bindings - cast = "Binding=address;Binding=address" (sequences.cpp)
int LuaPlaySequence(const LuaMadeSimple::Lua& lua) {
    auto manager = static_cast<uintptr_t>(lua.get_integer());
    std::wstring tag = Wide(std::string(lua.get_string()));
    int type = static_cast<int>(lua.get_integer());
    std::wstring list = lua.is_string() ? Wide(std::string(lua.get_string())) : L"";
    std::vector<std::pair<std::wstring, void*>> cast;
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find(L';', at);
        if (end == std::wstring::npos) end = list.size();
        auto item = list.substr(at, end - at);
        auto eq = item.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            auto addr = std::wcstoull(item.c_str() + eq + 1, nullptr, 0);
            if (addr) cast.emplace_back(item.substr(0, eq), reinterpret_cast<void*>(addr));
        }
        at = end + 1;
    }
    std::wstring music = lua.is_string() ? Wide(std::string(lua.get_string())) : L"";
    lua.set_bool(sequences::Play(reinterpret_cast<void*>(manager), tag, type, cast, music));
    return 1;
}

// WaCaNative_OpenPopup(a screen's address - the HUD's CurrentScreen, widget id) -> true / false: one of the game's
// popups (EWidgetsIds: 86 = the fresh-kill pile) opened the way a dialogue's create-screen action opens it (screens.cpp)
int LuaOpenPopup(const LuaMadeSimple::Lua& lua) {
    auto screen = static_cast<uintptr_t>(lua.get_integer());
    int id = static_cast<int>(lua.get_integer());
    lua.set_bool(screens::OpenPopup(reinterpret_cast<void*>(screen), id));
    return 1;
}

// WaCaNative_TextFallback(on) -> true / false: a role a line names that this save has no cat for is shown as its name
// in words instead of "{THE ROLE}" (fill.cpp)
int LuaTextFallback(const LuaMadeSimple::Lua& lua) {
    lua.set_bool(fill::SetFallback(lua.get_bool()));
    return 1;
}

// WaCaNative_FillText(text [, asShown]) -> the text as the game fills it in this save (its role / player / clan fill;
// asShown: with the fallback for roles with no cat), or
// nil when the anchors disagree (fill.cpp)
int LuaFillText(const LuaMadeSimple::Lua& lua) {
    std::wstring in = Wide(std::string(lua.get_string()));
    bool asShown = lua.is_bool() ? lua.get_bool() : false;
    std::wstring out;
    if (!fill::Fill(in, out, asShown)) {
        lua.set_nil();
        return 1;
    }
    lua.set_string(Narrow(out));
    return 1;
}

// WaCaNative_AddExperience(level component address, amount) -> true / false: EXP the way the game adds it (hunt.cpp)
int LuaAddExperience(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    auto amount = static_cast<float>(lua.get_number());
    lua.set_bool(hunt::AddExperience(reinterpret_cast<void*>(addr), amount));
    return 1;
}

// WaCaNative_AddBond(ENarrativeRole value, points) -> true / false: bond with a story cat, the game's way (bonds.cpp)
int LuaAddBond(const LuaMadeSimple::Lua& lua) {
    auto role = static_cast<uint8_t>(lua.get_integer());
    auto amount = static_cast<int32_t>(lua.get_integer());
    lua.set_bool(bonds::AddPersonal(role, amount));
    return 1;
}

// WaCaNative_AddResource(a quest resource component's address, amount) -> its new value, or nil (hunt.cpp)
int LuaAddResource(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    auto amount = static_cast<float>(lua.get_number());
    float now = 0;
    if (hunt::AddResource(reinterpret_cast<void*>(addr), amount, &now)) lua.set_number(now); else lua.set_nil();
    return 1;
}

// WaCaNative_HookAreas(a live movement-modifier area's address) -> true / false: AreaOverlap events (areas.cpp)
int LuaHookAreas(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    lua.set_bool(areas::HookFrom(reinterpret_cast<void*>(addr)));
    return 1;
}

// WaCaNative_CombatExperience(tuning manager address, enemy kind name) -> the EXP a fight against it gives, or -1
int LuaCombatExperience(const LuaMadeSimple::Lua& lua) {
    auto addr = static_cast<uintptr_t>(lua.get_integer());
    std::wstring kind = Wide(std::string(lua.get_string()));
    lua.set_integer(hunt::CombatExperience(reinterpret_cast<void*>(addr), kind));
    return 1;
}

// WaCaNative_RebuildLook(cat look component's address) -> true / false: the game builds that cat's whole look again
// from its own data (appearance.cpp) - with the look hook, any WaCa look goes in with it
int LuaRebuildLook(const LuaMadeSimple::Lua& lua) {
    auto* comp = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    void* data = comp ? comp->GetValuePtrByPropertyName(STR("CustomData")) : nullptr;
    lua.set_bool(data && appearance::Rebuild(comp, data));
    return 1;
}

// WaCaNative_CopyLook(to look component's address, from look component's address) -> true / false: the game builds
// one cat's whole look from ANOTHER cat's data (its CustomData) - a ceremony's stand-in dressed as the cat we cast
int LuaCopyLook(const LuaMadeSimple::Lua& lua) {
    auto* to = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    auto* from = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    void* data = nullptr;
    if (from) {
        auto* prop = from->GetPropertyByNameInChain(STR("CustomData"));
        data = prop ? prop->ContainerPtrToValuePtr<void>(from) : nullptr;
    }
    lua.set_bool(to && data && appearance::Rebuild(to, data));
    return 1;
}

// WaCaNative_ImportLook(look component's address, text) -> true / false: the component's CustomData read from the
// game's own text form of it - "(ClanType=ShadowClan,Role=Leader,BodyValue=6,...)", as the save keeps every story
// cat's look (NPCData.NarrativeRoles) - then the whole look built from it: a stand-in dressed as a cat that isn't in
// the world (another clan's, at a Gathering)
int LuaImportLook(const LuaMadeSimple::Lua& lua) {
    auto* comp = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    std::wstring text = Wide(std::string(lua.get_string()));
    bool ok = false;
    if (comp) {
        auto* prop = comp->GetPropertyByNameInChain(STR("CustomData"));
        // (a stand-in's look component - a subclass: found in the chain, not by name - VM 2026-10-31)
        void* data = prop ? prop->ContainerPtrToValuePtr<void>(comp) : nullptr;
        const TCHAR* end = nullptr;
        if (prop && data) {
            end = prop->ImportText(text.c_str(), data, 0, comp, nullptr);
            ok = end != nullptr && appearance::Rebuild(comp, data);
        }
        if (!ok)
            logx::Line(L"[look] import into %s: property %p, data %p, text read %s, rebuilt %s", comp->GetName().c_str(), prop, data,
                       end ? L"yes" : L"NO", ok ? L"yes" : L"no");
    }
    lua.set_bool(ok);
    return 1;
}

// WaCaNative_ExportLook(look component's address) -> its CustomData in the game's own text form
// ("(ClanType=RiverClan,BodyValue=6,...)" - what ImportLook reads), or nil
int LuaExportLook(const LuaMadeSimple::Lua& lua) {
    auto* comp = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    if (comp) {
        auto* prop = comp->GetPropertyByNameInChain(STR("CustomData"));
        void* data = prop ? prop->ContainerPtrToValuePtr<void>(comp) : nullptr;
        if (data) {
            FString out;
            prop->ExportTextItem_Direct(out, data, nullptr, comp, 0);
            lua.set_string(Narrow(*out ? *out : STR("")));
            return 1;
        }
    }
    lua.set_nil();
    return 1;
}

// WaCaNative_WidgetRect(a UMG widget's address) -> left, top, right, bottom in desktop pixels (Slate's absolute space),
// or nil: where the widget was last drawn. FGeometry keeps everything in fields reflection doesn't see (its UE 5.7
// layout: Size 0x0, Scale 0x8, AbsolutePosition 0xC, Position 0x14, AccumulatedRenderTransform 0x1C = a 2x2 matrix +
// a translation; 0x38 in all), so a Lua copy of it comes back all zeros - the mouse's hit tests on the game's menus
// (the clan crests, the key icons) need it read here.
int LuaWidgetRect(const LuaMadeSimple::Lua& lua) {
    auto* widget = reinterpret_cast<UObject*>(static_cast<uintptr_t>(lua.get_integer()));
    UFunction* fn = widget ? widget->GetFunctionByNameInChain(STR("GetCachedGeometry")) : nullptr;
    if (!fn) {
        lua.set_nil();
        return 1;
    }
    alignas(16) unsigned char geometry[0x38]{};
    widget->ProcessEvent(fn, geometry);
    float sizeX, sizeY, m00, m01, m10, m11, tx, ty;
    std::memcpy(&sizeX, geometry + 0x0, 4);
    std::memcpy(&sizeY, geometry + 0x4, 4);
    std::memcpy(&m00, geometry + 0x1C, 4);
    std::memcpy(&m01, geometry + 0x20, 4);
    std::memcpy(&m10, geometry + 0x24, 4);
    std::memcpy(&m11, geometry + 0x28, 4);
    std::memcpy(&tx, geometry + 0x2C, 4);
    std::memcpy(&ty, geometry + 0x30, 4);
    if (sizeX <= 0 || sizeY <= 0) {
        lua.set_nil();
        return 1;
    }
    // the far corner through the accumulated transform (local -> absolute: P * M + translation)
    float x1 = sizeX * m00 + sizeY * m10 + tx, y1 = sizeX * m01 + sizeY * m11 + ty;
    lua.set_number(std::min(tx, x1));
    lua.set_number(std::min(ty, y1));
    lua.set_number(std::max(tx, x1));
    lua.set_number(std::max(ty, y1));
    return 4;
}

// WaCaNative_CursorPos() -> x, y: the mouse in desktop pixels (the same space as WaCaNative_WidgetRect)
int LuaCursorPos(const LuaMadeSimple::Lua& lua) {
    long x = 0, y = 0;
    keys::CursorPos(x, y);
    lua.set_number(x);
    lua.set_number(y);
    return 2;
}

// WaCaNative_BoundKeys(command) -> "F, LeftMouseButton": the command's keyboard / mouse keys as they are now
// (keybinds.ini's, else the game's own; "" = unknown) - the controls screen's clash check (input.cpp)
int LuaBoundKeys(const LuaMadeSimple::Lua& lua) {
    if (!lua.is_string()) lua.throw_error("WaCaNative_BoundKeys(command)");
    std::string out;
    for (auto& k : input::BoundKeys(Wide(std::string(lua.get_string())))) {
        if (!out.empty()) out += ", ";
        out += Narrow(k);
    }
    lua.set_string(out);
    return 1;
}

// WaCaNative_CloneTableRow(DataTable address, existing row, new row) -> true / false: a new row in one of the game's
// tables, a deep copy of an existing one (tables.cpp) - mod rows in the game's own lists (pattern / marking grids)
int LuaCloneTableRow(const LuaMadeSimple::Lua& lua) {
    auto* table = reinterpret_cast<void*>(static_cast<uintptr_t>(lua.get_integer()));
    std::wstring from = Wide(std::string(lua.get_string()));
    std::wstring to = Wide(std::string(lua.get_string()));
    lua.set_bool(tables::CloneRow(table, from, to));
    return 1;
}

// WaCaNative_SetTableField(DataTable address, row, field, value) -> true / false: text, a soft object path
// ("/Game/Folder/Asset.Asset"), an enum value name, a bool or an integer
int LuaSetTableField(const LuaMadeSimple::Lua& lua) {
    auto* table = reinterpret_cast<void*>(static_cast<uintptr_t>(lua.get_integer()));
    std::wstring row = Wide(std::string(lua.get_string()));
    std::wstring field = Wide(std::string(lua.get_string()));
    std::wstring value = Wide(std::string(lua.get_string()));
    lua.set_bool(tables::SetField(table, row, field, value));
    return 1;
}

int LuaCommand(const LuaMadeSimple::Lua& lua) {
    if (!lua.is_string()) lua.throw_error("WaCaNative_Command(text)");
    bool ok = commands::Run(Wide(std::string(lua.get_string())));
    lua.set_bool(ok);
    return 1;
}

}  // namespace

namespace bridge {

void Install() {
    Hook::FCallbackOptions once{true, true, STR("WaCaCore"), STR("GameThread")};
    auto id = Hook::RegisterEngineTickPreCallback(
        [](auto&, UEngine*, float, bool) {
            g_gameThread = std::this_thread::get_id();
            g_haveGameThread.store(true, std::memory_order_release);
            logx::Line(L"[bridge] game thread found (engine tick)");
        },
        once);
    if (id == Hook::ERROR_ID) logx::Line(L"[bridge] no engine tick hook - events are OFF");
    Hook::RegisterProcessEventPreCallback([](auto&, UObject* c, UFunction* f, void* p) { OnProcessEvent(false, c, f, p); },
                                          {false, true, STR("WaCaCore"), STR("EventsPre")});
    Hook::RegisterProcessEventPostCallback([](auto&, UObject* c, UFunction* f, void* p) { OnProcessEvent(true, c, f, p); },
                                           {false, true, STR("WaCaCore"), STR("EventsPost")});
    combat::Install();
    logx::Line(L"[bridge] ProcessEvent callbacks installed");
}

void LuaStarted(const std::wstring& modName, LuaMadeSimple::Lua& lua) {
    g_lastLuaStart.store(NowMs(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_loadingMutex);
        g_loadingMods[modName] = NowMs();
    }
    auto state = std::make_unique<ModState>();
    state->name = modName;
    state->mod = get_mod_ref(lua);
    state->thread = &lua.new_thread();
    lua.registry().make_ref();                // anchors the thread (new_thread left it on the stack)
    lua.register_function("WaCaNative_Watch", &LuaWatch);
    lua.register_function("WaCaNative_DevTrace", &LuaDevTrace);
    lua.register_function("WaCaNative_Ready", &LuaReady);
    lua.register_function("WaCaNative_ModsLoading", &LuaModsLoading);
    lua.register_function("WaCaNative_Broadcast", &LuaBroadcast);
    lua.register_function("WaCaNative_LastBroadcast", &LuaLastBroadcast);
    lua.register_function("WaCaNative_Command", &LuaCommand);
    lua.register_function("WaCaNative_DevDelegate", &LuaDevDelegate);
    lua.register_function("WaCaNative_RebuildLook", &LuaRebuildLook);
    lua.register_function("WaCaNative_CopyLook", &LuaCopyLook);
    lua.register_function("WaCaNative_ImportLook", &LuaImportLook);
    lua.register_function("WaCaNative_ExportLook", &LuaExportLook);
    lua.register_function("WaCaNative_CloneTableRow", &LuaCloneTableRow);
    lua.register_function("WaCaNative_WidgetRect", &LuaWidgetRect);
    lua.register_function("WaCaNative_CursorPos", &LuaCursorPos);
    lua.register_function("WaCaNative_BoundKeys", &LuaBoundKeys);
    lua.register_function("WaCaNative_DevAxisBindings", &LuaDevAxisBindings);
    lua.register_function("WaCaNative_DevWatchAxis", &LuaDevWatchAxis);
    lua.register_function("WaCaNative_DevWatchAddress", &LuaDevWatchAddress);
    lua.register_function("WaCaNative_DevReadFloats", &LuaDevReadFloats);
    lua.register_function("WaCaNative_DevVirtual", &LuaDevVirtual);
    lua.register_function("WaCaNative_DevVtable", &LuaDevVtable);
    lua.register_function("WaCaNative_DevFindBinding", &LuaDevFindBinding);
    lua.register_function("WaCaNative_DevReadInts", &LuaDevReadInts);
    lua.register_function("WaCaNative_HookPrey", &LuaHookPrey);
    lua.register_function("WaCaNative_HookHunt", &LuaHookHunt);
    lua.register_function("WaCaNative_HuntDetect", &LuaHuntDetect);
    lua.register_function("WaCaNative_PlaySequence", &LuaPlaySequence);
    lua.register_function("WaCaNative_OpenPopup", &LuaOpenPopup);
    lua.register_function("WaCaNative_FillText", &LuaFillText);
    lua.register_function("WaCaNative_TextFallback", &LuaTextFallback);
    lua.register_function("WaCaNative_HookAreas", &LuaHookAreas);
    lua.register_function("WaCaNative_AddExperience", &LuaAddExperience);
    lua.register_function("WaCaNative_AddBond", &LuaAddBond);
    lua.register_function("WaCaNative_AddResource", &LuaAddResource);
    lua.register_function("WaCaNative_CombatExperience", &LuaCombatExperience);
    lua.register_function("WaCaNative_DevTraceCommandAxis", &LuaDevTraceCommandAxis);
    lua.register_function("WaCaNative_SetTableField", &LuaSetTableField);
    {
        // (the loader thread waits for a delivery in progress - at most 2 s: UE4SS may hold a lock of its own here
        // that the delivery's Lua is waiting for; then on without it, as before)
        bool locked = g_statesMutex.try_lock_for(std::chrono::seconds(2));
        if (!locked) logx::Line(L"[bridge] %s starting while an event runs - added without waiting", modName.c_str());
        g_states.push_back(std::move(state));
        if (locked) g_statesMutex.unlock();
    }
}

void LuaStopping(LuaMadeSimple::Lua& lua) {
    g_lastLuaStart.store(NowMs(), std::memory_order_release);   // (a reload: hold events from here, not from its start)
    // a mod's own menu kept every key from the game (SwallowAll) and its Lua is going - the keys go back, or the game
    // would take no key at all (a reload with the controls screen open, VM 2026-11-01)
    keys::SwallowAll(false);
    bool locked = g_statesMutex.try_lock_for(std::chrono::seconds(2));     // (as in LuaStarted)
    struct Unlock { bool on; ~Unlock() { if (on) g_statesMutex.unlock(); } } unlockAtEnd{locked};
    if (!locked) logx::Line(L"[bridge] a mod stopping while an event runs - removed without waiting");
    LuaMod* mod = get_mod_ref(lua);
    for (auto it = g_states.begin(); it != g_states.end(); ++it) {
        if ((*it)->mod != mod) continue;
        ModState* s = it->get();
        {
            std::lock_guard<std::mutex> lock(g_watchesMutex);
            std::erase_if(g_watches, [s](const Watch& w) { return w.state == s; });
            g_watchCount = g_watches.size();
        }
        logx::Line(L"[bridge] %s stopped", s->name.c_str());
        g_states.erase(it);
        return;
    }
}

void EmitLook(void* component, void* part, const char* what) {
    if (!OnGameThread() || g_states.empty()) return;
    auto* comp = static_cast<UObject*>(component);
    std::wstring partName = static_cast<UObject*>(part)->GetFullName();
    int n = WideCharToMultiByte(CP_UTF8, 0, partName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, partName.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    std::lock_guard<std::recursive_timed_mutex> lock(g_statesMutex);
    for (size_t i = 0; i < g_states.size(); ++i) Deliver(*g_states[i], "CatLookPart", comp, {what, utf8});
}

void Emit(const char* event, std::initializer_list<std::string> args) {
    if (!OnGameThread()) {
        logx::Line(L"[bridge] event %s raised off the game thread - dropped", Wide(event).c_str());
        return;
    }
    std::lock_guard<std::recursive_timed_mutex> lock(g_statesMutex);
    for (size_t i = 0; i < g_states.size(); ++i) Deliver(*g_states[i], event, nullptr, args);
}

}  // namespace bridge
