#pragma once

// The cat look's build primitives, hooked (see appearance.cpp)
namespace appearance {
void Install();
// the game builds a cat's whole look again from its data (SetDefaultCustomData) - false when that is unavailable
bool Rebuild(void* component, const void* customData);
}
