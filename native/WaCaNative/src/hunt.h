#pragma once
#include <string>

// A hunt's instakill succeeds only by chance (see hunt.cpp)
namespace hunt {
// hook the prey component's landing from a live USwtAdventurePreyComponent (its vtable); once
bool HookFrom(void* preyComponent);
// HuntRoll|<0/1>|<base chance %>|<critical miss %>|<CharacterType value>=<factor>,...
void Configure(const std::wstring& command);
// HuntLoud: a no-op now (the landing no longer checks loud ground - the prey is already caught there)
void SetLoud(bool loud, double windowSeconds);
// HuntFail|<prey full name>: the pounce under way on that prey fails (it spotted the cat during the walk)
void Fail(const std::wstring& preyName);
// one pass of the game's own prey detection (sight / sound / cone, as its AI runs it) on a detection component, while
// the game has it off (the pounce walk): the function its behaviour-tree service UBTService_DetectionTask runs every
// tick, found through a live service's vtable; false when the anchors disagree
bool Detect(void* service, void* detectionComponent, int detectionType);
// add EXP to a level component (USwtQuestLevelComponent, the player state's) the way the game does - its resource
// "add value" virtual (level-ups included); false when the anchors disagree
bool AddExperience(void* levelComponent, float amount);
// add to any quest resource component (USwtQuestResourceComponent: the fresh-kill pile, level, health, stamina) the
// game's way - its add-value virtual, checked per class against the anchors; *now = the value after (+0x10C)
bool AddResource(void* component, float amount, float* now);
// the EXP a fight against an enemy kind gives (the game's combat base experience table, as the tuning manager loaded
// it), or -1
int CombatExperience(void* tuningManager, const std::wstring& kind);
}
