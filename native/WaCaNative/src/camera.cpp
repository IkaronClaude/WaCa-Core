// Mouse look on the game's own camera. The game turns its camera only from the right stick, through a stick
// pipeline: a dead zone, a response curve (|v|^e), a clamp to [-2, 2], then * turn speed * frame time. A mouse fed
// into that axis (the old [Axes] MouseX mapping) turns at most at stick speed and feels sticky. Here the mouse is
// read the standard UE way (APlayerController::GetInputMouseDelta - the MouseX / MouseY keys, rebindable) and goes
// into the game's own turn value already divided by turn speed * frame time - so the game turns the camera by exactly
// delta * sensitivity degrees: no dead zone, curve or clamp, and the stick still works the game's way on top.
//
// Traced 2026-10-25 (tickets.md, mouse look): the camera update (component, input source, dt) asks the turn function
// (component -> the sum of the camera / movement axes, shaped and clamped) and adds turn * component+0x1CC (turn
// speed) * dt to the camera actor's input rotator (+0x3A0). That rotator is only honoured while the camera sees input:
// written from outside (before or after the update) the view didn't move (VM: the arrow keys turned it 16.8 deg, a
// direct write turned nothing) - hence going through the turn value. The player's invert settings live on the input
// source (+0x8B3 set = yaw inverted); the idle check after the update recentres the camera unless component+0x220
// (the idle time) stays 0.
// Anchors (relocation framework): the update by the string "LookAtYaw" + prologue, confirmed by its body asking for
// the camera axes (mov byte ptr [rbp+67h], 3Eh twice); the turn function = the one function the update calls that asks
// for the camera-right axis (mov byte ptr [rsp+70h], 3Fh). Anything missing = mouse look off (logged); the stick
// still works.
#include "camera.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "MinHook.h"
#include "log.h"
#include "scan.h"

using namespace RC;
using namespace RC::Unreal;

namespace {

using UpdateFn = void(__fastcall*)(void* component, void* input, float dt);
using TurnFn = float(__fastcall*)(void* component, float deadZone);
using AxisFn = float(__fastcall*)(void* input, const uint8_t* command);
UpdateFn g_update = nullptr;
TurnFn g_turn = nullptr;
AxisFn g_pitchAxis = nullptr;               // the raw axis getter the update asks for camera-up (vtable +0x1008)
uint8_t* g_pitchAxisAt = nullptr;
bool g_pitchTried = false;

std::atomic<bool> g_on{false};
std::atomic<float> g_yawScale{0.f};
std::atomic<float> g_pitchScale{0.f};

// Camera-relative movement. The game already moves the cat relative to the camera (0x1417A4940: forward / right,
// normalised, along the camera's forward projected on the ground), but its turn function adds movement-right to the
// CAMERA turn - steering swings the camera round, so right / left circle instead of strafing ("tank" controls). On:
// while the turn function runs, movement-right reads 0 for it - only the camera axis (stick / mouse) turns the
// camera, and right / left move the cat sideways relative to it. Its getter: the dead-zone axis getter (vtable
// +0x1000), hooked from the live vtable like the pitch one.
using DeadZoneAxisFn = float(__fastcall*)(void* input, const uint8_t* command, float deadZone);
DeadZoneAxisFn g_moveAxis = nullptr;
bool g_moveTried = false;
bool g_inTurn = false;
std::atomic<bool> g_cameraRelative{false};
constexpr int kDeadZoneGetterSlot = 0x1000;
constexpr uint8_t kMoveRight = 61;
void HookMoveAxis(void* input);

// layout of the 2026-10-25 build (see the file comment)
constexpr int kTurnSpeed = 0x1CC, kPitchSpeed = 0x1D0, kCurve = 0x1D4, kIdleTime = 0x220;
constexpr int kInvertYaw = 0x8B3, kNoInvertPitch = 0x8B4;
constexpr int kAxisGetterSlot = 0x1008;
constexpr uint8_t kCameraUp = 62;

// this frame's update, for the functions it calls (all on the game thread)
void* g_frameComponent = nullptr;
void* g_frameInput = nullptr;
float g_frameDt = 0.f;
// the pitch speed swapped for this frame (restored after the update), see PitchAxisDetour
bool g_speedSwapped = false;
float g_savedPitchSpeed = 0.f;

// weak: a level change destroys it; looked up again (once) when gone
FWeakObjectPtr g_pc;
UFunction* g_mouseDelta = nullptr;

// the first live instance of a class (FindFirstOf can return the class default object - VM 2026-10-25)
UObject* FirstLive(const CharType* className) {
    std::vector<UObject*> all;
    UObjectGlobals::FindAllOf(className, all);
    for (UObject* o : all)
        if (o && !o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject))) return o;
    return nullptr;
}

bool MouseDelta(float& dx, float& dy) {
    UObject* pc = g_pc.Get();
    if (!pc || !g_mouseDelta) {
        pc = FirstLive(STR("PlayerController"));
        g_pc = FWeakObjectPtr(pc);
        g_mouseDelta = pc ? pc->GetFunctionByNameInChain(STR("GetInputMouseDelta")) : nullptr;
        if (!g_mouseDelta) return false;
    }
    struct { float x, y; } params{};
    pc->ProcessEvent(g_mouseDelta, &params);
    dx = params.x;
    dy = params.y;
    return true;
}

// Pitch: the update asks the raw axis getter for camera-up (v), clamps it to [-1, 1] and adds
// sign(v) * |v|^curve * dt * pitch speed - capped at dt * pitch speed per frame. So for a frame with mouse movement
// the getter answers sign(total) (|v| = 1: the curve does nothing) and the pitch speed becomes |total| / dt, total
// being the stick's own degrees plus the mouse's: the game adds exactly that, still inside its own pitch limits.
// The mouse's direction mirrors the old MouseY:-0.3 axis mapping through the getter's invert rule (camera-up is
// flipped unless the player's "don't invert" flag is set).
float __fastcall PitchAxisDetour(void* input, const uint8_t* command) {
    float v = g_pitchAxis(input, command);
    // dev (WACA_TRACE_RAW=1): who reads which raw axis - each new (caller, command) once, as an exe address (for
    // finding where movement is applied: camera-relative movement, tickets.md 19b)
    static const bool trace = [] {
        wchar_t on[4] = {};
        return GetEnvironmentVariableW(L"WACA_TRACE_RAW", on, 4) && on[0] == L'1';
    }();
    if (trace && command) {
        static std::vector<std::pair<uintptr_t, int>> seen;
        auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        auto key = std::make_pair(ret, static_cast<int>(*command));
        if (seen.size() < 64 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            logx::Line(L"[camera] raw axis %d = %.3f read from 0x%llX", key.second, v,
                       static_cast<unsigned long long>(ret - base + 0x140000000ull));
        }
    }
    if (!g_on || input != g_frameInput || !g_frameComponent || !command || *command != kCameraUp || g_frameDt <= 0.f)
        return v;
    float dx = 0.f, dy = 0.f;
    if (!MouseDelta(dx, dy) || dy == 0.f) return v;
    auto* comp = static_cast<uint8_t*>(g_frameComponent);
    auto* in = static_cast<uint8_t*>(input);
    float speed = *reinterpret_cast<float*>(comp + kPitchSpeed);
    float curve = *reinterpret_cast<float*>(comp + kCurve);
    float stick = std::fmax(-1.f, std::fmin(1.f, v));
    float stickDegrees = (stick > 0.f ? 1.f : stick < 0.f ? -1.f : 0.f) * std::pow(std::fabs(stick), curve) * g_frameDt * speed;
    float mouseDegrees = -dy * (in[kNoInvertPitch] ? 1.f : -1.f) * g_pitchScale;
    float total = stickDegrees + mouseDegrees;
    static bool reported = false;
    if (!reported) {
        reported = true;
        logx::Line(L"[camera] first mouse pitch: delta %.3f -> %.2f deg (+ stick %.2f; pitch speed %.1f, dt %.4f)", dy,
                   mouseDegrees, stickDegrees, speed, g_frameDt);
    }
    if (total == 0.f) return 0.f;
    if (!g_speedSwapped) {
        g_speedSwapped = true;
        g_savedPitchSpeed = speed;
    }
    *reinterpret_cast<float*>(comp + kPitchSpeed) = std::fabs(total) / g_frameDt;
    *reinterpret_cast<float*>(comp + kIdleTime) = 0.f;
    return total > 0.f ? 1.f : -1.f;
}

// the axis getter is a virtual of the input source: hooked from its vtable on the first update (anchored: the
// update calls exactly that slot, and the function there reads the invert flags)
void HookPitchAxis(void* input) {
    g_pitchTried = true;
    auto* fn = (*reinterpret_cast<uint8_t***>(input))[kAxisGetterSlot / 8];
    if (!scan::BodyContains(fn, "80 BB B4 08 00 00 00", 1)) {
        logx::Line(L"[camera] pitch axis getter at %p doesn't read the invert flag - anchors disagree, mouse pitch OFF", fn);
        return;
    }
    bool ok = MH_CreateHook(fn, reinterpret_cast<void*>(&PitchAxisDetour), reinterpret_cast<void**>(&g_pitchAxis)) == MH_OK &&
              MH_EnableHook(fn) == MH_OK;
    if (ok) g_pitchAxisAt = fn;
    logx::Line(L"[camera] pitch axis getter at %p (confirmed: reads the invert flag)%s", fn, ok ? L"" : L" - could not hook");
}

void __fastcall UpdateDetour(void* component, void* input, float dt) {
    if (!g_pitchTried && input) HookPitchAxis(input);
    if (!g_moveTried && input) HookMoveAxis(input);
    g_frameComponent = component;
    g_frameInput = input;
    g_frameDt = dt;
    g_update(component, input, dt);
    if (g_speedSwapped) {
        *reinterpret_cast<float*>(static_cast<uint8_t*>(component) + kPitchSpeed) = g_savedPitchSpeed;
        g_speedSwapped = false;
    }
    g_frameInput = nullptr;
    g_frameComponent = nullptr;
}

// the game's turn (stick) + the mouse's yaw in turn units: the caller multiplies by turn speed * dt
float __fastcall MoveAxisDetour(void* input, const uint8_t* command, float deadZone) {
    float v = g_moveAxis(input, command, deadZone);
    if (!(g_inTurn && g_cameraRelative && command && *command == kMoveRight)) return v;
    static bool reported = false;
    if (v != 0.f && !reported) {
        reported = true;
        logx::Line(L"[camera] movement-right %.2f kept out of the camera turn (camera-relative movement)", v);
    }
    return 0.f;
}

// the dead-zone getter, from the live vtable (anchored: the turn function calls that slot - checked at Install -
// and the getter reads the yaw invert flag, cmp byte ptr [rdi+8B3h], 0)
void HookMoveAxis(void* input) {
    g_moveTried = true;
    auto* fn = (*reinterpret_cast<uint8_t***>(input))[kDeadZoneGetterSlot / 8];
    if (!scan::BodyContains(fn, "80 BF B3 08 00 00 00", 1)) {
        logx::Line(L"[camera] dead-zone axis getter at %p doesn't read the invert flag - camera-relative movement OFF", fn);
        return;
    }
    bool ok = MH_CreateHook(fn, reinterpret_cast<void*>(&MoveAxisDetour), reinterpret_cast<void**>(&g_moveAxis)) == MH_OK &&
              MH_EnableHook(fn) == MH_OK;
    logx::Line(L"[camera] dead-zone axis getter at %p (confirmed: reads the invert flag)%s", fn, ok ? L"" : L" - could not hook");
}

float __fastcall TurnDetour(void* component, float deadZone) {
    // camera-relative movement: movement-right doesn't turn the camera (see g_cameraRelative)
    g_inTurn = true;
    float turn = g_turn(component, deadZone);
    g_inTurn = false;
    if (!g_on || !component || !g_frameInput || g_frameDt <= 0.f) return turn;
    float dx = 0.f, dy = 0.f;
    if (!MouseDelta(dx, dy) || dx == 0.f) return turn;
    auto* comp = static_cast<uint8_t*>(component);
    float speed = *reinterpret_cast<float*>(comp + kTurnSpeed);
    if (!(std::fabs(speed) > 1e-4f)) return turn;
    auto* in = static_cast<uint8_t*>(g_frameInput);
    float degrees = dx * g_yawScale * (in[kInvertYaw] ? -1.f : 1.f);
    float mouse = degrees / (speed * g_frameDt);
    static bool reported = false;
    if (!reported) {
        reported = true;
        logx::Line(L"[camera] first mouse turn: delta %.3f -> %.2f deg (turn speed %.1f, dt %.4f): turn %.3f %+.3f", dx,
                   degrees, speed, g_frameDt, turn, mouse);
    }
    *reinterpret_cast<float*>(comp + kIdleTime) = 0.f;      // the player is looking around: no recentring
    return turn + mouse;
}

}  // namespace

namespace camera {

void Install() {
    auto* update = scan::FunctionUsing("LookAtYaw", "40 55 53 41 55 41 56");
    if (!update) {
        logx::Line(L"[camera] update: not found (anchor \"LookAtYaw\") - mouse look OFF");
        return;
    }
    if (!scan::BodyContains(update, "C6 45 67 3E", 2)) {
        logx::Line(L"[camera] update at %p: its body doesn't ask for the camera axes - anchors disagree, mouse look OFF", update);
        return;
    }
    if (!scan::BodyContains(update, "FF 90 08 10 00 00", 1)) {        // call qword ptr [rax + 1008h]: the pitch axis
        logx::Line(L"[camera] update at %p doesn't call the pitch axis slot - mouse pitch OFF (yaw unaffected)", update);
        g_pitchTried = true;
    }
    auto* turn = scan::CalleeWhere(update, "C6 44 24 70 3F");
    if (!turn) {
        logx::Line(L"[camera] turn function: no single callee of the update asks for the camera-right axis - mouse look OFF");
        return;
    }
    if (!scan::BodyContains(turn, "FF 90 00 10 00 00", 2)) {           // call qword ptr [rax + 1000h]: its axes
        logx::Line(L"[camera] turn at %p doesn't call the axis getter slot - camera-relative movement OFF", turn);
        g_moveTried = true;
    }
    bool ok = MH_CreateHook(update, reinterpret_cast<void*>(&UpdateDetour), reinterpret_cast<void**>(&g_update)) == MH_OK &&
              MH_CreateHook(turn, reinterpret_cast<void*>(&TurnDetour), reinterpret_cast<void**>(&g_turn)) == MH_OK &&
              MH_EnableHook(update) == MH_OK && MH_EnableHook(turn) == MH_OK;
    logx::Line(L"[camera] update at %p, turn at %p (confirmed: update asks for the camera axes, turn is its camera-right "
               L"callee)%s", update, turn, ok ? L"" : L" - could not hook");
}

void SetCameraRelativeMove(bool on) {
    if (g_cameraRelative.exchange(on) != on)
        logx::Line(L"[camera] camera-relative movement %s", on ? L"on (right / left move sideways, only the camera axis turns the camera)" : L"off (the game's: right / left turn the camera)");
}

void SetMouseLook(bool on, float yawScale, float pitchScale) {
    g_yawScale = yawScale;
    g_pitchScale = pitchScale;
    if (g_on.exchange(on) != on) logx::Line(L"[camera] mouse look %s (yaw %.3f, pitch %.3f per unit)", on ? L"on" : L"off", yawScale, pitchScale);
}

}  // namespace camera
