#include <doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "core/params.h"
#include "dsp/biquad.h"
#include "engine/channel_eq.h"

using namespace ion;

TEST_CASE("params: normalized round-trip for every mixer parameter") {
  for (const ParamInfo& p : mixerParams()) {
    for (float n : {0.0f, 0.1f, 0.25f, 0.5f, 0.9f, 1.0f}) {
      const float v = fromNormalized(p, n);
      CHECK(v >= p.min);
      CHECK(v <= p.max);
      if (p.curve != Curve::Toggle && p.curve != Curve::Choice && !(p.curve == Curve::Fader && n == 0.0f))
        CHECK(toNormalized(p, v) == doctest::Approx(n).epsilon(1e-4));
    }
    CHECK(clampValue(p, p.def) == p.def);
  }
}

TEST_CASE("params: fader taper") {
  const ParamInfo& f = mixerParam(kFader);
  CHECK(fromNormalized(f, 1.0f) == doctest::Approx(6.0f));
  CHECK(fromNormalized(f, 0.5f) == doctest::Approx(-6.04f).epsilon(0.01));
  CHECK(fromNormalized(f, 0.0f) <= kFaderMinDb);
  CHECK(faderGain(kFaderMinDb) == 0.0f);
  CHECK(faderGain(-6.0f) == doctest::Approx(0.501f).epsilon(0.002));
  CHECK(formatValue(f, kFaderMinDb) == "−∞ dB");
  CHECK(formatValue(f, -6.0f) == "-6.0 dB");
}

TEST_CASE("params: stepping and formatting") {
  const ParamInfo& pan = mixerParam(kPan);
  CHECK(formatValue(pan, 0.0f) == "C");
  CHECK(formatValue(pan, -0.25f) == "L25");
  CHECK(stepValue(pan, 0.0f, 1, false) == doctest::Approx(0.05f));
  CHECK(stepValue(pan, 1.0f, 1, true) == 1.0f);  // clamped
  const ParamInfo& mute = mixerParam(kMute);
  CHECK(stepValue(mute, 0.0f, 1, false) == 1.0f);
  CHECK(stepValue(mute, 1.0f, -1, false) == 0.0f);
  const ParamInfo& freq = mixerParam(kB1Freq);
  CHECK(formatValue(freq, 2500.0f) == "2.50 kHz");
  CHECK(stepValue(freq, 1000.0f, 1, true) > stepValue(freq, 1000.0f, 1, false));
}

TEST_CASE("params: keys") {
  CHECK(paramKey({2, kFader}, mixerParam(kFader)) == "track3.fader");
  CHECK(paramKey({kMasterScope, kFader}, mixerParam(kFader)) == "master.fader");
  CHECK(paramKey({0, kB1Gain}, mixerParam(kB1Gain)) == "track1.eq.b1.gain");
}

// ---- RBJ biquads ----

namespace {

constexpr double kSr = 48000;

// Measured gain of the filter at f: run a sine through it, compare RMS.
double measuredDb(const dsp::BiquadCoeffs& c, double f) {
  dsp::Biquad bq;
  bq.setCoeffs(c);
  double in = 0, out = 0;
  const int n = 48000, settle = 12000;
  for (int i = 0; i < n; ++i) {
    const float x = static_cast<float>(std::sin(2 * std::numbers::pi * f * i / kSr));
    const float y = bq.process(x);
    if (i >= settle) {
      in += double(x) * x;
      out += double(y) * y;
    }
  }
  return 10 * std::log10(out / in);
}

}  // namespace

TEST_CASE("RBJ: bell peaks at its gain, at its centre frequency") {
  for (double g : {-12.0, -3.0, 6.0, 12.0}) {
    const auto c = dsp::design(dsp::FilterShape::Peak, 1000, 1.0, g, kSr);
    CHECK(20 * std::log10(dsp::magnitude(c, 1000, kSr)) == doctest::Approx(g).epsilon(1e-3));
    CHECK(measuredDb(c, 1000) == doctest::Approx(g).epsilon(0.01));
    CHECK(std::fabs(20 * std::log10(dsp::magnitude(c, 20, kSr))) < 0.1);  // flat far away
  }
}

TEST_CASE("RBJ: shelves reach their gain, half way at the corner") {
  const auto lo = dsp::design(dsp::FilterShape::LowShelf, 200, 0.707, 9, kSr);
  CHECK(20 * std::log10(dsp::magnitude(lo, 20, kSr)) == doctest::Approx(9).epsilon(0.02));
  CHECK(20 * std::log10(dsp::magnitude(lo, 200, kSr)) == doctest::Approx(4.5).epsilon(0.02));
  CHECK(std::fabs(20 * std::log10(dsp::magnitude(lo, 10000, kSr))) < 0.05);
  const auto hi = dsp::design(dsp::FilterShape::HighShelf, 5000, 0.707, -6, kSr);
  CHECK(20 * std::log10(dsp::magnitude(hi, 20000, kSr)) == doctest::Approx(-6).epsilon(0.05));
  CHECK(20 * std::log10(dsp::magnitude(hi, 5000, kSr)) == doctest::Approx(-3).epsilon(0.02));
}

TEST_CASE("RBJ: Butterworth high/low-pass are −3 dB at the cutoff") {
  const auto hp = dsp::design(dsp::FilterShape::HighPass, 100, 0.70710678, 0, kSr);
  CHECK(20 * std::log10(dsp::magnitude(hp, 100, kSr)) == doctest::Approx(-3.01).epsilon(0.01));
  CHECK(measuredDb(hp, 100) == doctest::Approx(-3.01).epsilon(0.02));
  CHECK(20 * std::log10(dsp::magnitude(hp, 50, kSr)) == doctest::Approx(-12.3).epsilon(0.03));  // 12 dB/oct
  const auto lp = dsp::design(dsp::FilterShape::LowPass, 5000, 0.70710678, 0, kSr);
  CHECK(20 * std::log10(dsp::magnitude(lp, 5000, kSr)) == doctest::Approx(-3.01).epsilon(0.01));
}

// ---- channel EQ ----

namespace {

std::vector<float> mixerDefaults() {
  std::vector<float> v(kMixerParamCount);
  for (int i = 0; i < kMixerParamCount; ++i) v[static_cast<std::size_t>(i)] = mixerParams()[static_cast<std::size_t>(i)].def;
  return v;
}

double eqMeasuredDb(const EqSettings& s, double f) {
  ChannelEq eq(kSr);
  std::vector<float> l(64), r(64);
  double in = 0, out = 0;
  for (int b = 0; b < 750; ++b) {
    for (int i = 0; i < 64; ++i) l[i] = r[i] = static_cast<float>(std::sin(2 * std::numbers::pi * f * (b * 64 + i) / kSr));
    const std::vector<float> x = l;
    eq.process(l.data(), r.data(), 64, s);
    if (b > 200)
      for (int i = 0; i < 64; ++i) {
        in += double(x[i]) * x[i];
        out += double(l[i]) * l[i];
      }
  }
  return 10 * std::log10(out / in);
}

}  // namespace

TEST_CASE("channel EQ: defaults are transparent") {
  const auto v = mixerDefaults();
  const EqSettings s = EqSettings::fromParams(v.data());
  for (double f : {30.0, 400.0, 3000.0, 15000.0}) CHECK(std::fabs(eqResponseDb(s, f, kSr)) < 1e-6);
  ChannelEq eq(kSr);
  std::vector<float> l(64, 0.5f), r(64, -0.25f);
  eq.process(l.data(), r.data(), 64, s);
  CHECK(l[10] == 0.5f);
  CHECK(r[10] == -0.25f);
}

TEST_CASE("channel EQ: what you hear matches the curve the UI draws") {
  auto v = mixerDefaults();
  v[kB1Gain] = 6;
  v[kB1Freq] = 1000;
  v[kLsGain] = -4;
  v[kHpOn] = 1;
  v[kHpFreq] = 80;
  v[kHpSlope] = 1;  // 24 dB/oct
  const EqSettings s = EqSettings::fromParams(v.data());
  for (double f : {60.0, 150.0, 1000.0, 5000.0}) CHECK(eqMeasuredDb(s, f) == doctest::Approx(eqResponseDb(s, f, kSr)).epsilon(0.02));

  v[kEqBypass] = 1;
  const EqSettings bypassed = EqSettings::fromParams(v.data());
  CHECK(eqResponseDb(bypassed, 1000, kSr) == 0.0);
  CHECK(std::fabs(eqMeasuredDb(bypassed, 1000)) < 0.01);
}

TEST_CASE("channel EQ: big parameter jumps don't click, no NaN") {
  auto v = mixerDefaults();
  ChannelEq eq(kSr);
  std::vector<float> l(64), r(64);
  float worstStep = 0, prev = 0;
  for (int b = 0; b < 400; ++b) {
    if (b == 100) { v[kB1Gain] = 18; v[kB1Freq] = 200; v[kB1Q] = 8; }
    if (b == 200) { v[kHpOn] = 1; v[kHpFreq] = 2000; }
    if (b == 300) { v[kEqBypass] = 1; }
    for (int i = 0; i < 64; ++i) l[i] = r[i] = 0.25f * static_cast<float>(std::sin(2 * std::numbers::pi * 220 * (b * 64 + i) / kSr));
    eq.process(l.data(), r.data(), 64, EqSettings::fromParams(v.data()));
    for (int i = 0; i < 64; ++i) {
      REQUIRE(std::isfinite(l[i]));
      worstStep = std::max(worstStep, std::fabs(l[i] - prev));
      prev = l[i];
    }
  }
  // A 220 Hz sine boosted to at most +18 dB (×7.9, ≈2.0 peak) moves at most
  // 2·π·220/48000·2.0 ≈ 0.058 per sample; a click would be far larger.
  CHECK(worstStep < 0.08f);
}
