#pragma once

// Movement-modifier areas' overlaps as Lua events (see areas.cpp)
namespace areas {
// hook the areas' overlap begin / end from a live ASwtAdventureMovementModifierArea (its vtable); once
bool HookFrom(void* area);
}
