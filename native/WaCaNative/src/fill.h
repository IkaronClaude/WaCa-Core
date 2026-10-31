#pragma once
#include <string>

// The game's own dialogue-text fill (role / player / clan placeholders), run on any text (see fill.cpp)
namespace fill {
// out = the text as the game fills it in this save (asShown: with our fallback too); false when the anchors disagree
bool Fill(const std::wstring& in, std::wstring& out, bool asShown = false);
// on: every line the game fills gets, for a role this save has no cat for, the role's name in words ("the ambitious
// apprentice") instead of the placeholder; false when the anchors disagree
bool SetFallback(bool on);
}
