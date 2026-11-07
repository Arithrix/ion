#pragma once

#include <string>

namespace ion {

// "0.0.1"
const char* versionNumber();
// "ion 0.0.1 (a1b2c3d, 2026-11-06)", with "-dirty" after the hash for an uncommitted tree.
std::string versionLine();

}  // namespace ion
