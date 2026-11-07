#include "engine/channel_eq.h"

#include <algorithm>
#include <cmath>

namespace ion {
namespace {

using dsp::FilterShape;

// Butterworth Q values: one stage for 12 dB/oct, two staggered stages for 24.
constexpr double kQ12 = 0.70710678;
constexpr double kQ24a = 0.54119610;
constexpr double kQ24b = 1.30656296;

// Per-block glide factor: ~20 ms time constant at 64-frame blocks, 48 kHz.
constexpr float kGlide = 0.064f;

float glideLog(float cur, float target) { return cur * std::pow(target / cur, kGlide); }
float glideLin(float cur, float target) { return cur + (target - cur) * kGlide; }
float snap(float cur, float target, float eps) { return std::fabs(cur - target) < eps ? target : cur; }

bool on(const float* v, MixerParam p) { return v[p] >= 0.5f; }

}  // namespace

EqSettings EqSettings::fromParams(const float* v) {
  EqSettings s;
  s.bypass = on(v, kEqBypass);
  s.hpOn = on(v, kHpOn);
  s.hpFreq = v[kHpFreq];
  s.hp24 = v[kHpSlope] >= 0.5f;
  s.lpOn = on(v, kLpOn);
  s.lpFreq = v[kLpFreq];
  s.lowShelf = {on(v, kLsOn), v[kLsFreq], v[kLsGain], v[kLsQ]};
  s.bell1 = {on(v, kB1On), v[kB1Freq], v[kB1Gain], v[kB1Q]};
  s.bell2 = {on(v, kB2On), v[kB2Freq], v[kB2Gain], v[kB2Q]};
  s.highShelf = {on(v, kHsOn), v[kHsFreq], v[kHsGain], v[kHsQ]};
  return s;
}

double eqResponseDb(const EqSettings& s, double f, double sr) {
  if (s.bypass) return 0.0;
  double mag = 1.0;
  if (s.hpOn) {
    if (s.hp24) {
      mag *= dsp::magnitude(dsp::design(FilterShape::HighPass, s.hpFreq, kQ24a, 0, sr), f, sr);
      mag *= dsp::magnitude(dsp::design(FilterShape::HighPass, s.hpFreq, kQ24b, 0, sr), f, sr);
    } else {
      mag *= dsp::magnitude(dsp::design(FilterShape::HighPass, s.hpFreq, kQ12, 0, sr), f, sr);
    }
  }
  auto band = [&](const EqSettings::Band& b, FilterShape shape) {
    if (b.on && b.gain != 0.0f) mag *= dsp::magnitude(dsp::design(shape, b.freq, b.q, b.gain, sr), f, sr);
  };
  band(s.lowShelf, FilterShape::LowShelf);
  band(s.bell1, FilterShape::Peak);
  band(s.bell2, FilterShape::Peak);
  band(s.highShelf, FilterShape::HighShelf);
  if (s.lpOn) mag *= dsp::magnitude(dsp::design(FilterShape::LowPass, s.lpFreq, kQ12, 0, sr), f, sr);
  return 20.0 * std::log10(std::max(mag, 1e-9));
}

ChannelEq::ChannelEq(double sampleRate) : sr_(sampleRate) {}

void ChannelEq::reset(const EqSettings& s) {
  cur_.hpFreq = s.hpFreq;
  cur_.lpFreq = s.lpFreq;
  cur_.hp24 = s.hp24;
  auto band = [](const EqSettings::Band& b) { return EqSettings::Band{true, b.freq, b.on ? b.gain : 0.0f, b.q}; };
  cur_.ls = band(s.lowShelf);
  cur_.b1 = band(s.bell1);
  cur_.b2 = band(s.bell2);
  cur_.hs = band(s.highShelf);
  cur_.hpMix = s.hpOn ? 1.0f : 0.0f;
  cur_.lpMix = s.lpOn ? 1.0f : 0.0f;
  cur_.wetMix = s.bypass ? 0.0f : 1.0f;
  for (auto& ch : filters_)
    for (auto& f : ch) f.reset();
  updateCoefficients();
  first_ = false;
}

void ChannelEq::updateCoefficients() {
  const double hpQ1 = cur_.hp24 ? kQ24a : kQ12;
  coeffs_[0] = dsp::design(FilterShape::HighPass, cur_.hpFreq, hpQ1, 0, sr_);
  coeffs_[1] = dsp::design(FilterShape::HighPass, cur_.hpFreq, kQ24b, 0, sr_);
  coeffs_[2] = dsp::design(FilterShape::LowShelf, cur_.ls.freq, cur_.ls.q, cur_.ls.gain, sr_);
  coeffs_[3] = dsp::design(FilterShape::Peak, cur_.b1.freq, cur_.b1.q, cur_.b1.gain, sr_);
  coeffs_[4] = dsp::design(FilterShape::Peak, cur_.b2.freq, cur_.b2.q, cur_.b2.gain, sr_);
  coeffs_[5] = dsp::design(FilterShape::HighShelf, cur_.hs.freq, cur_.hs.q, cur_.hs.gain, sr_);
  coeffs_[6] = dsp::design(FilterShape::LowPass, cur_.lpFreq, kQ12, 0, sr_);
  for (auto& ch : filters_)
    for (std::size_t i = 0; i < 7; ++i) ch[i].setCoeffs(coeffs_[i]);
}

void ChannelEq::process(float* left, float* right, std::size_t frames, const EqSettings& s) {
  if (first_) reset(s);

  // ---- glide toward targets (once per call; calls are ≤ 64 frames) ----
  bool changed = false;
  auto glideF = [&](float& cur, float target, bool log) {
    const float before = cur;
    cur = log ? glideLog(cur, target) : glideLin(cur, target);
    cur = snap(cur, target, log ? target * 1e-4f : 1e-3f);
    changed |= cur != before;
  };
  auto glideBand = [&](EqSettings::Band& cur, const EqSettings::Band& target) {
    glideF(cur.freq, target.freq, true);
    glideF(cur.q, target.q, true);
    glideF(cur.gain, target.on ? target.gain : 0.0f, false);
  };
  glideF(cur_.hpFreq, s.hpFreq, true);
  glideF(cur_.lpFreq, s.lpFreq, true);
  glideBand(cur_.ls, s.lowShelf);
  glideBand(cur_.b1, s.bell1);
  glideBand(cur_.b2, s.bell2);
  glideBand(cur_.hs, s.highShelf);
  if (cur_.hp24 != s.hp24) {
    cur_.hp24 = s.hp24;
    changed = true;
  }
  if (changed) updateCoefficients();

  // Switch crossfades move linearly over ~10 ms; computed per sample below.
  const float mixStep = 1.0f / 480.0f;
  const float hpTarget = s.hpOn ? 1.0f : 0.0f;
  const float lpTarget = s.lpOn ? 1.0f : 0.0f;
  const float wetTarget = s.bypass ? 0.0f : 1.0f;

  // Fast path: everything flat and settled.
  const bool bandsFlat = cur_.ls.gain == 0 && cur_.b1.gain == 0 && cur_.b2.gain == 0 && cur_.hs.gain == 0;
  if (bandsFlat && cur_.hpMix == 0 && hpTarget == 0 && cur_.lpMix == 0 && lpTarget == 0) {
    cur_.wetMix = wetTarget;  // nothing to crossfade: wet == dry
    active_.fill(false);
    return;
  }

  const bool hpActive = cur_.hpMix > 0 || hpTarget > 0;
  const bool lpActive = cur_.lpMix > 0 || lpTarget > 0;
  const std::array<bool, 7> active = {hpActive, hpActive && cur_.hp24, cur_.ls.gain != 0, cur_.b1.gain != 0,
                                      cur_.b2.gain != 0, cur_.hs.gain != 0, lpActive};
  for (std::size_t st = 0; st < 7; ++st) {
    if (active[st] && !active_[st])
      for (auto& ch : filters_) ch[st].reset();
    active_[st] = active[st];
  }

  float* io[2] = {left, right};
  float hpMix = 0, lpMix = 0, wetMix = 0;
  for (int ch = 0; ch < 2; ++ch) {
    auto& f = filters_[static_cast<std::size_t>(ch)];
    hpMix = cur_.hpMix;
    lpMix = cur_.lpMix;
    wetMix = cur_.wetMix;
    float* x = io[ch];
    for (std::size_t i = 0; i < frames; ++i) {
      hpMix += std::clamp(hpTarget - hpMix, -mixStep, mixStep);
      lpMix += std::clamp(lpTarget - lpMix, -mixStep, mixStep);
      wetMix += std::clamp(wetTarget - wetMix, -mixStep, mixStep);
      const float dry = x[i];
      float y = dry;
      if (active[0]) {
        float h = f[0].process(y);
        if (active[1]) h = f[1].process(h);
        y = hpMix * h + (1 - hpMix) * y;
      }
      if (active[2]) y = f[2].process(y);
      if (active[3]) y = f[3].process(y);
      if (active[4]) y = f[4].process(y);
      if (active[5]) y = f[5].process(y);
      if (active[6]) y = lpMix * f[6].process(y) + (1 - lpMix) * y;
      x[i] = wetMix * y + (1 - wetMix) * dry;
    }
  }
  cur_.hpMix = hpMix;
  cur_.lpMix = lpMix;
  cur_.wetMix = wetMix;
}

}  // namespace ion
