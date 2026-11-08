#include "ui/envelope_view.h"

#include <algorithm>
#include <cmath>

#include "engine/envelope_params.h"

namespace ion::ui {

std::string envelopeShape(const float* v, int width) {
  static const char* levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
  const float a = v[kEnvAttack] <= 0.1f ? 0 : v[kEnvAttack];
  const bool holdFull = v[kEnvHold] >= kEnvFull, releaseOff = v[kEnvRelease] >= kEnvOff;
  const float h = holdFull ? 0 : (v[kEnvHold] <= 1 ? 0 : v[kEnvHold]);
  const float d = v[kEnvDecay], s = v[kEnvSustain];
  // The held part (or the sustain) gets a fixed share of the picture.
  const float shown = std::max(50.0f, (a + h + d) * 0.5f);
  const float r = releaseOff ? shown : v[kEnvRelease];
  const float total = a + h + (holdFull ? 0 : d) + shown + r;
  std::string out;
  for (int x = 0; x < width; ++x) {
    const float t = total * (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
    float level;
    float held = holdFull ? 1.0f : s;
    if (t < a) level = t / a;
    else if (t < a + h) level = 1;
    else if (!holdFull && t < a + h + d) level = s + (1 - s) * std::pow(1e-3f, (t - a - h) / d);
    else if (t < total - r) level = held;
    else level = releaseOff ? held : held * std::pow(1e-3f, (t - (total - r)) / r);
    out += levels[std::clamp(static_cast<int>(std::lround(level * 8)), 0, 8)];
  }
  return out;
}

}  // namespace ion::ui
