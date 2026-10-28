#pragma once
#include <string>

// The commands Lua sends with WaCaNative.Command("<Verb>|<arg>|<arg>...") (see commands.cpp)
namespace commands {
// runs one command on the calling (game) thread; false for an unknown verb
bool Run(const std::wstring& cmd);
}
