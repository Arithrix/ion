#pragma once

// RBJ Audio EQ Cookbook biquads (Robert Bristow-Johnson), transposed direct form II.

namespace ion::dsp {

struct BiquadCoeffs {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // normalized so a0 = 1
};

enum class FilterShape { LowPass, HighPass, Peak, LowShelf, HighShelf };

// f0 in Hz, gainDb for Peak/shelves, q is Q (for shelves: the cookbook's Q form).
BiquadCoeffs design(FilterShape shape, double f0, double q, double gainDb, double sampleRate);

// |H(e^jw)| at frequency f.
double magnitude(const BiquadCoeffs& c, double f, double sampleRate);

class Biquad {
 public:
  void setCoeffs(const BiquadCoeffs& c) { c_ = c; }
  void reset() { z1_ = z2_ = 0; }

  float process(float x) {
    const float y = c_.b0 * x + z1_;
    z1_ = c_.b1 * x - c_.a1 * y + z2_;
    z2_ = c_.b2 * x - c_.a2 * y;
    return y;
  }

 private:
  BiquadCoeffs c_;
  float z1_ = 0, z2_ = 0;
};

}  // namespace ion::dsp
