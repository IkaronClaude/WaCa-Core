#pragma once
#include <string>
#include <utility>
#include <vector>

// The game's own scene sequences (rank-up ceremonies, clan meetings ...) played the game's way (see sequences.cpp)
namespace sequences {
// play the sequence registered under a tag, of a sequencer type (ESwtEngineSequencerType: 3 = narrative progression,
// the ceremonies), through the sequencer manager's own play - with replacements: a binding's name -> one of our
// actors in its place (the game's ActorsReplaced). False when the anchors disagree.
bool Play(void* manager, const std::wstring& tag, int type, const std::vector<std::pair<std::wstring, void*>>& cast);
}
