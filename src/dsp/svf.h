#pragma once

// A topology-preserving state-variable filter (Andrew Simper's, from his
// "Linear Trapezoidal Integrated SVF" note): stable under fast modulation,
// one set of coefficients for low-, high-, band-pass and notch.

#include <cmath>
#include <numbers>

namespace ion::dsp {

struct SvfCoeffs {
  float a1 = 1, a2 = 0, a3 = 0, k = 2;
  // fc in Hz; res 0…1 (Q 0.5 … ~20).
  static SvfCoeffs make(float fc, float res, float sampleRate) {
    SvfCoeffs c;
    const float g = std::tan(std::numbers::pi_v<float> * std::fmin(fc, sampleRate * 0.49f) / sampleRate);
    c.k = 2.0f - 1.95f * res;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
  }
};

class Svf {
 public:
  enum class Mode { LowPass, HighPass, BandPass, Notch };
  float process(float v0, const SvfCoeffs& c, Mode m) {
    const float v3 = v0 - ic2_;
    const float v1 = c.a1 * ic1_ + c.a2 * v3;
    const float v2 = ic2_ + c.a2 * ic1_ + c.a3 * v3;
    ic1_ = 2 * v1 - ic1_;
    ic2_ = 2 * v2 - ic2_;
    switch (m) {
      case Mode::LowPass: return v2;
      case Mode::HighPass: return v0 - c.k * v1 - v2;
      case Mode::BandPass: return v1;
      case Mode::Notch: return v0 - c.k * v1;
    }
    return v2;
  }
  void reset() { ic1_ = ic2_ = 0; }

 private:
  float ic1_ = 0, ic2_ = 0;
};

}  // namespace ion::dsp
