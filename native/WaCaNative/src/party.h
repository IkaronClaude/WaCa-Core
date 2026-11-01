#pragma once
#include <string>

// The fight's party: who fights beside your cat (see party.cpp)
namespace party {
// Party|29,43: the next fights' party = those narrative roles; Party| = the game's own. False when it can't be hooked.
bool Set(const std::wstring& list);
}
