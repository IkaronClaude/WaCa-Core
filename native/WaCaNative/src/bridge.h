#pragma once
#include <initializer_list>
#include <string>

namespace RC::LuaMadeSimple {
class Lua;
}

// The event bus between the game and the Lua mods (see bridge.cpp)
namespace bridge {
// once the engine is up (on_unreal_init): the ProcessEvent callbacks + the game-thread probe
void Install();
// a Lua mod started / is stopping: its WaCaNative table, its event thread, its watches
void LuaStarted(const std::wstring& modName, RC::LuaMadeSimple::Lua& lua);
void LuaStopping(RC::LuaMadeSimple::Lua& lua);
// a native event with string arguments, to every Lua mod (game thread only: dropped and logged elsewhere)
void Emit(const char* event, std::initializer_list<std::string> args);
// CatLookPart(component, what, part's full name): a look primitive wrote to one part (appearance.cpp)
void EmitLook(void* component, void* part, const char* what);
}
