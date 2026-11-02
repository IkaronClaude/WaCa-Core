#pragma once

// A guard against an engine crash at start-up / on a level load (see gcguard.cpp)
namespace gcguard {
void Install();
}
