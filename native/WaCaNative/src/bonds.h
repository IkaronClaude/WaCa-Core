#pragma once
#include <cstdint>

// Bonds with the story cats, the game's own way (see bonds.cpp)
namespace bonds {
// add `amount` bond points to a narrative role (ENarrativeRole value) - the game's AddPersonalBonding (it saves and
// raises the bond rank like a dialogue answer does); false when its anchors don't match this game version
bool AddPersonal(uint8_t role, int32_t amount);
}
