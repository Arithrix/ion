#pragma once

// The uniform envelope's parameters (DECISIONS D-50), the same in every
// engine's table: same keys ("amp.attack"), ranges, curves and labels. Only
// the defaults differ, to suit each instrument.

#include "core/params.h"
#include "dsp/ahdsr.h"

namespace ion {

enum EnvParam : int { kEnvAttack, kEnvHold, kEnvDecay, kEnvSustain, kEnvRelease, kEnvParamCount };

struct EnvDefaults {
  float attack, hold, decay, sustain, release;  // ms, ms, ms, 0–1, ms; hold kEnvFull / release kEnvOff for the ends
};
inline constexpr float kEnvFull = 10000.0f;  // hold "full": until released (or the sample ends)
inline constexpr float kEnvOff = 20000.0f;   // release "off": no fade; the sound ends by itself

namespace detail {
inline constexpr float kFine = 1.0f / 60.0f, kCoarse = 1.0f / 12.0f;
}

// The envelope's five parameters. `filter` picks the filter envelope's keys
// ("filter.attack", "F attack") instead of the amp's.
constexpr ParamInfo envelopeParam(bool filter, int which, const EnvDefaults& d) {
  using detail::kCoarse;
  using detail::kFine;
  switch (which) {
    case kEnvAttack:
      return {filter ? "filter.attack" : "amp.attack", filter ? "F attack" : "Attack", 0.1f, 10000, d.attack, Curve::Log, Unit::Ms, kFine,
              kCoarse, nullptr, nullptr, "0"};
    case kEnvHold:
      return {filter ? "filter.hold" : "amp.hold", filter ? "F hold" : "Hold", 1, kEnvFull, d.hold, Curve::Log, Unit::Ms, kFine, kCoarse,
              nullptr, "full", "0"};
    case kEnvDecay:
      return {filter ? "filter.decay" : "amp.decay", filter ? "F decay" : "Decay", 1, 20000, d.decay, Curve::Log, Unit::Ms, kFine, kCoarse};
    case kEnvSustain:
      return {filter ? "filter.sustain" : "amp.sustain", filter ? "F sustain" : "Sustain", 0, 1, d.sustain, Curve::Linear, Unit::Percent,
              0.02f, 0.1f};
    default:
      return {filter ? "filter.release" : "amp.release", filter ? "F release" : "Release", 1, kEnvOff, d.release, Curve::Log, Unit::Ms,
              kFine, kCoarse, nullptr, "off"};
  }
}

// Velocity → amp, which sits with the amp envelope everywhere.
constexpr ParamInfo velAmpParam(float def) {
  return {"vel.amp", "Vel→amp", 0, 1, def, Curve::Linear, Unit::Percent, 0.05f, 0.25f};
}
inline float velocityGain(float amount, float velocity01) { return 1.0f - amount + amount * velocity01 * velocity01; }

// Coefficients from the five values, in EnvParam order.
inline dsp::Ahdsr::Coeffs envelopeCoeffs(const float* v) {
  return dsp::Ahdsr::make(v[kEnvAttack] <= 0.1f ? 0.0f : v[kEnvAttack], v[kEnvHold] <= 1.0f ? 0.0f : v[kEnvHold], v[kEnvHold] >= kEnvFull,
                          v[kEnvDecay], v[kEnvSustain], v[kEnvRelease], v[kEnvRelease] >= kEnvOff);
}

}  // namespace ion
