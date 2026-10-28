#pragma once
#include <string>

// Rows added to the game's data tables (see tables.cpp)
namespace tables {
// a new row `to`: a deep copy of row `from` of the same table
bool CloneRow(void* table, const std::wstring& from, const std::wstring& to);
// one field of a row: text, a soft object path, an enum value name / number, a bool, an integer
bool SetField(void* table, const std::wstring& row, const std::wstring& field, const std::wstring& value);
}
