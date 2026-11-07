#pragma once

#include <array>
#include <cstddef>

#include "core/params.h"
#include "dsp/biquad.h"

namespace ion {

// The settings of one channel EQ as plain values. Built from a track's mixer
// parameters; shared by the DSP and the UI's response curve so they can't disagree.
struct EqSettings {
  bool bypass = false;
  bool hpOn = false, lpOn = false;
  float hpFreq = 40, lpFreq = 18000;
  bool hp24 = false;
  struct Band {
    bool on = true;
    float freq = 1000, gain = 0, q = 1;
  };
  Band lowShelf, bell1, bell2, highShelf;

  static EqSettings fromParams(const float* mixerValues);  // kMixerParamCount values
};

// Magnitude response of the whole EQ in dB at frequency f (what the UI draws).
double eqResponseDb(const EqSettings& s, double f, double sampleRate);

// HP → low shelf → bell 1 → bell 2 → high shelf → LP, stereo.
// Parameters glide to their targets over ~20 ms (updated every block), bands
// switched off fade their gain to 0 dB, and the HP/LP/bypass switches crossfade,
// so nothing clicks when edited live.
class ChannelEq {
 public:
  explicit ChannelEq(double sampleRate = 48000.0);
  void reset(const EqSettings& s);
  // Processes `frames` (≤ 64) samples in place toward target settings `s`.
  void process(float* left, float* right, std::size_t frames, const EqSettings& s);

 private:
  struct Smoothed {
    float hpFreq, lpFreq;
    EqSettings::Band ls, b1, b2, hs;
    float hpMix, lpMix, wetMix;  // 0…1 crossfades for the switches
    bool hp24;
  };
  void updateCoefficients();

  double sr_;
  Smoothed cur_{};
  bool first_ = true;
  // [channel][stage]: hp1, hp2, ls, b1, b2, hs, lp
  std::array<std::array<dsp::Biquad, 7>, 2> filters_{};
  std::array<dsp::BiquadCoeffs, 7> coeffs_{};
  // Stages skipped while inactive (flat bells/shelves, switched-off HP/LP).
  // A flat RBJ peak/shelf keeps zero state, so resetting on re-activation is exact.
  std::array<bool, 7> active_{};
};

}  // namespace ion
