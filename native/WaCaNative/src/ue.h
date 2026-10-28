// The few Unreal Engine 5.7 types the native layer touches, laid out as the shipping game uses them
// (no case-preserving names: FName is 8 bytes). Verified against the game's code - see PROJECT_PLAN.md "Input".
#pragma once
#include <cstdint>

namespace ue {

struct FName {
    uint32_t ComparisonIndex = 0;
    uint32_t Number = 0;
    bool operator==(const FName& o) const { return ComparisonIndex == o.ComparisonIndex && Number == o.Number; }
};

template <typename T>
struct TArray {
    T* Data = nullptr;
    int32_t Num = 0;
    int32_t Max = 0;
};

struct FString {
    wchar_t* Data = nullptr;
    int32_t Num = 0;          // characters including the terminator
    int32_t Max = 0;
    const wchar_t* c_str() const { return Data && Num > 0 ? Data : L""; }
};

// FKey: the key's name + a lazily resolved TSharedPtr<FKeyDetails> (null is valid: the engine looks it up on demand)
struct FKey {
    FName KeyName;
    void* KeyDetails = nullptr;
    void* KeyDetailsRef = nullptr;
};
static_assert(sizeof(FKey) == 0x18);

struct FInputActionKeyMapping {
    FName ActionName;
    uint8_t ModifierBits = 0;  // bShift : 1, bCtrl : 1, bAlt : 1, bCmd : 1
    uint8_t Pad[7] = {};
    FKey Key;
};
static_assert(sizeof(FInputActionKeyMapping) == 0x28);

struct FInputAxisKeyMapping {
    FName AxisName;
    float Scale = 1.0f;
    uint8_t Pad[4] = {};
    FKey Key;
};
static_assert(sizeof(FInputAxisKeyMapping) == 0x28);

}  // namespace ue

namespace game {

// One default binding as the game builds it (SwtPlayerInput, 0x58 bytes): Kind 0 = action, 1 = axis; Command is the
// SwtEngine ECommands value. The game's apply step registers Action (Kind 0) or Axis (Kind 1) on the UPlayerInput.
struct CommandMapping {
    uint8_t Kind;
    uint8_t Command;
    uint8_t Pad[6];
    ue::FInputActionKeyMapping Action;
    ue::FInputAxisKeyMapping Axis;
};
static_assert(sizeof(CommandMapping) == 0x58);

constexpr uint8_t KindAction = 0;
constexpr uint8_t KindAxis = 1;
constexpr uint8_t KindDropped = 0xFE;   // anything but 0 / 1 is skipped by the game's apply loop

}  // namespace game
