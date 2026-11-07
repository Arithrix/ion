#include "dsp/biquad.h"

#include <cmath>
#include <complex>
#include <numbers>

namespace ion::dsp {

BiquadCoeffs design(FilterShape shape, double f0, double q, double gainDb, double sr) {
  const double w0 = 2.0 * std::numbers::pi * f0 / sr;
  const double cw = std::cos(w0);
  const double sw = std::sin(w0);
  const double alpha = sw / (2.0 * q);
  const double A = std::pow(10.0, gainDb / 40.0);

  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (shape) {
    case FilterShape::LowPass:
      b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FilterShape::HighPass:
      b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case FilterShape::Peak:
      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
      a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      break;
    case FilterShape::LowShelf: {
      const double s = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cw + s);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - s);
      a0 = (A + 1) + (A - 1) * cw + s;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - s;
      break;
    }
    case FilterShape::HighShelf: {
      const double s = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cw + s);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - s);
      a0 = (A + 1) - (A - 1) * cw + s;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - s;
      break;
    }
  }
  return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0),
          static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
}

double magnitude(const BiquadCoeffs& c, double f, double sr) {
  const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / sr);
  const std::complex<double> z2 = z1 * z1;
  const std::complex<double> num = static_cast<double>(c.b0) + static_cast<double>(c.b1) * z1 + static_cast<double>(c.b2) * z2;
  const std::complex<double> den = 1.0 + static_cast<double>(c.a1) * z1 + static_cast<double>(c.a2) * z2;
  return std::abs(num / den);
}

}  // namespace ion::dsp
