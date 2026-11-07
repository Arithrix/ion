#pragma once

#include <string>
#include <vector>

namespace ion::ui {

// A DX7 algorithm drawn with box-drawing characters, from dx7::topology(), so
// it always matches what the engine plays. Operators are numbered as on the
// DX7, carriers sit on the bottom row joined by the output line, and ↺ marks
// the operator that takes feedback. Laid out like Yamaha's chart: operator 1
// bottom left, each modulator above what it modulates.
//
//   boxed:  ┌─┐ ┌─┐↺       compact:  2  6↺
//           │2│ │6│                 │  │
//           └┬┘ └┬┘                 1  5
//           ┌┴┐ ┌┴┐  …              └──┘
//
// `algorithm` is 0-31. Lines are UTF-8, all the same display width.
std::vector<std::string> algorithmDiagram(int algorithm, bool boxed);

}  // namespace ion::ui
