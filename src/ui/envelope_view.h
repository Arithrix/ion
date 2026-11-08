#pragma once

// Drawing the uniform envelope (D-50): the same picture on every instrument.

#include <string>

namespace ion::ui {

// An AHDSR as block characters over `width` columns, from its five values
// (attack, hold, decay ms, sustain 0–1, release ms; hold "full" and release
// "off" at the top of their ranges).
std::string envelopeShape(const float* values, int width);

}  // namespace ion::ui
