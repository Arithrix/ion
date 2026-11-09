#include "engine/fx/effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "engine/engine.h"

namespace ion {
namespace {

constexpr float kTimeFine = 1.0f / 60.0f;
constexpr float kTimeCoarse = 1.0f / 12.0f;
constexpr float kLogFine = 1.0f / 120.0f;
constexpr float kLogCoarse = 1.0f / 20.0f;
constexpr float kSr = static_cast<float>(kSampleRate);

inline float softClip(float x) {
  x = std::clamp(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// 4-point Hermite between x1 and x2 (x0, x3 the outer points).
inline float hermite(float x0, float x1, float x2, float x3, float f) {
  const float c = (x2 - x0) * 0.5f;
  const float v = x1 - x2;
  const float w = c + v;
  const float a = w + v + (x3 - x1) * 0.5f;
  const float b = w + a;
  return ((a * f - b) * f + c) * f + x1;
}

// Reads `delay` samples behind `write` in a power-of-two ring.
inline float readRing(const std::vector<float>& buf, int mask, int write, double delay) {
  const double pos = static_cast<double>(write) - delay;
  const auto i = static_cast<int>(std::floor(pos));
  const auto f = static_cast<float>(pos - i);
  auto at = [&](int k) { return buf[static_cast<std::size_t>(k & mask)]; };
  return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), f);
}

float msCoeff(float ms) { return std::exp(-1.0f / (std::max(ms, 0.01f) * kSr / 1000.0f)); }

const char* const kDivisions[] = {"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"};
const char* const kKinds[] = {"plate", "hall", "room"};

const std::array<ParamInfo, Compressor::kParamCount> kCompressor = {{
    {"threshold", "Threshold", -60, 0, -18, Curve::Linear, Unit::Db, 0.5f, 3},
    {"ratio", "Ratio", 1, 20, 3, Curve::Log, Unit::Ratio, kTimeFine, kTimeCoarse},
    {"attack", "Attack", 0.1f, 100, 10, Curve::Log, Unit::Ms, kTimeFine, kTimeCoarse},
    {"release", "Release", 10, 2000, 150, Curve::Log, Unit::Ms, kTimeFine, kTimeCoarse},
    {"knee", "Knee", 0, 24, 6, Curve::Linear, Unit::Db, 0.5f, 3},
    {"makeup", "Makeup", 0, 24, 0, Curve::Linear, Unit::Db, 0.5f, 3},
    {"mix", "Mix", 0, 1, 1, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
}};

const char* const kDriveShapes[] = {"soft", "hard", "fold"};
const std::array<ParamInfo, Drive::kParamCount> kDriveTable = {{
    {"drive", "Drive", 0, 48, 12, Curve::Linear, Unit::Db, 0.5f, 3},
    {"shape", "Shape", 0, 2, 0, Curve::Choice, Unit::None, 1, 1, kDriveShapes},
    {"bias", "Bias", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"tone", "Tone", 500, 20000, 9000, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"level", "Level", -36, 6, -6, Curve::Linear, Unit::Db, 0.5f, 3},
    {"mix", "Mix", 0, 1, 1, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
}};

const std::array<ParamInfo, Chorus::kParamCount> kChorus = {{
    {"rate", "Rate", 0.05f, 5, 0.4f, Curve::Log, Unit::Hz, kTimeFine, kTimeCoarse},
    {"depth", "Depth", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"mix", "Mix", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"width", "Width", 0, 1, 0.8f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
}};

const std::array<ParamInfo, Delay::kParamCount> kDelay = {{
    {"sync", "Sync", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"division", "Division", 0, 11, 6, Curve::Choice, Unit::None, 1, 3, kDivisions},
    {"time", "Time", 1, 5000, 375, Curve::Log, Unit::Ms, kTimeFine / 2, kTimeCoarse},
    {"feedback", "Feedback", 0, 1, 0.4f, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"highcut", "High cut", 500, 20000, 8000, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"lowcut", "Low cut", 20, 2000, 80, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"pingpong", "Ping-pong", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"width", "Width", 0, 1, 1, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"mix", "Mix", 0, 1, 0.3f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
}};

const std::array<ParamInfo, Reverb::kParamCount> kReverb = {{
    {"type", "Type", 0, 2, 1, Curve::Choice, Unit::None, 1, 1, kKinds},
    {"size", "Size", 0, 1, 0.7f, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"decay", "Decay", 200, 20000, 2400, Curve::Log, Unit::Ms, kTimeFine, kTimeCoarse},
    {"predelay", "Pre-delay", 0, 200, 20, Curve::Linear, Unit::Ms, 1, 10},
    {"damping", "Damping", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"width", "Width", 0, 1, 0.9f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"mod", "Mod", 0, 1, 0.25f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"lowcut", "Low cut", 20, 1000, 200, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"highcut", "High cut", 1000, 20000, 9000, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"mix", "Mix", 0, 1, 0.25f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
}};

template <class E, std::size_t N>
void applyDefaults(E& e, const std::array<ParamInfo, N>& table) {
  for (std::size_t i = 0; i < N; ++i) e.setParam(static_cast<int>(i), table[i].def);
}

}  // namespace

// ---- registry -------------------------------------------------------------------------

const char* effectKey(EffectType type) {
  switch (type) {
    case EffectType::Compressor: return "compressor";
    case EffectType::Chorus: return "chorus";
    case EffectType::Delay: return "delay";
    case EffectType::Reverb: return "reverb";
    case EffectType::Drive: return "drive";
    case EffectType::None: break;
  }
  return "none";
}

const char* effectLabel(EffectType type) {
  switch (type) {
    case EffectType::Compressor: return "COMP";
    case EffectType::Chorus: return "CHORUS";
    case EffectType::Delay: return "DELAY";
    case EffectType::Reverb: return "REVERB";
    case EffectType::Drive: return "DRIVE";
    case EffectType::None: break;
  }
  return "—";
}

EffectType effectFromKey(const char* key) {
  for (int i = 1; i < kEffectTypes; ++i)
    if (std::strcmp(key, effectKey(static_cast<EffectType>(i))) == 0) return static_cast<EffectType>(i);
  return EffectType::None;
}

std::span<const ParamInfo> effectParams(EffectType type) {
  switch (type) {
    case EffectType::Compressor: return kCompressor;
    case EffectType::Chorus: return kChorus;
    case EffectType::Delay: return kDelay;
    case EffectType::Reverb: return kReverb;
    case EffectType::Drive: return kDriveTable;
    case EffectType::None: break;
  }
  return {};
}

std::unique_ptr<Effect> createEffect(EffectType type) {
  switch (type) {
    case EffectType::Compressor: return std::make_unique<Compressor>();
    case EffectType::Chorus: return std::make_unique<Chorus>();
    case EffectType::Delay: return std::make_unique<Delay>();
    case EffectType::Reverb: return std::make_unique<Reverb>();
    case EffectType::Drive: return std::make_unique<Drive>();
    case EffectType::None: break;
  }
  return nullptr;
}

std::span<const ParamInfo> Compressor::paramTable() { return kCompressor; }
std::span<const ParamInfo> Chorus::paramTable() { return kChorus; }
std::span<const ParamInfo> Drive::paramTable() { return kDriveTable; }

// ---- drive ---------------------------------------------------------------------------------

namespace {

// The shapes and their antiderivatives (for ADAA).
float shapeF(int s, float x) {
  switch (s) {
    case Drive::kHard: return std::clamp(x, -1.0f, 1.0f);
    case Drive::kFold: return std::sin(x * std::numbers::pi_v<float> * 0.5f);
    default: return std::tanh(x);
  }
}
float shapeAnti(int s, float x) {
  switch (s) {
    case Drive::kHard: return std::fabs(x) <= 1.0f ? 0.5f * x * x : std::fabs(x) - 0.5f;
    case Drive::kFold: return -std::cos(x * std::numbers::pi_v<float> * 0.5f) * (2.0f / std::numbers::pi_v<float>);
    default: {
      // log(cosh(x)), stable for large |x|
      const float a = std::fabs(x);
      return a + std::log1p(std::exp(-2.0f * a)) - 0.69314718f;
    }
  }
}

}  // namespace

Drive::Drive() {
  // A windowed-sinc half-band low-pass (cutoff at a quarter of the 2× rate),
  // Blackman window. Taps k = 0, 2, 4 … 46 are the non-zero outer ones; tap 23
  // (the middle) is ½ and every other odd tap is 0.
  constexpr int m = kTaps / 2;
  for (int j = 0; j < kPhase; ++j) {
    const int n = 2 * j;
    const double k = n - m;
    const double sinc = std::sin(0.5 * std::numbers::pi * k) / (std::numbers::pi * k);
    const double w = 0.42 - 0.5 * std::cos(2 * std::numbers::pi * n / (kTaps - 1)) + 0.08 * std::cos(4 * std::numbers::pi * n / (kTaps - 1));
    h_[static_cast<std::size_t>(j)] = static_cast<float>(sinc * w);
  }
  applyDefaults(*this, kDriveTable);
}

// Polyphase 2× around the shaper. Up: the even output is the 24 outer taps
// over the input history (×2 for the zero-stuffing), the odd one the input
// 11 samples ago (the middle tap). Down, keeping the odd sample: the outer
// taps over the shaped odd samples, plus ½ the shaped even sample from the middle.
float Drive::oversampled(Channel& c, float in) const {
  auto push = [&](std::array<float, 2 * kPhase>& h, float v) {
    h[static_cast<std::size_t>(c.pos)] = v;
    h[static_cast<std::size_t>(c.pos + kPhase)] = v;
  };
  push(c.up, in);
  const float* x = c.up.data() + c.pos + 1;  // oldest … newest
  float even = 0;
  for (int j = 0; j < kPhase; ++j) even += h_[static_cast<std::size_t>(j)] * x[kPhase - 1 - j];
  even *= 2.0f;
  const float odd = x[kPhase - 1 - kMid];
  push(c.downEven, shapeOne(c, even));
  push(c.downOdd, shapeOne(c, odd));
  const float* se = c.downEven.data() + c.pos + 1;
  const float* so = c.downOdd.data() + c.pos + 1;
  // Keeping the odd (latest) sample: the outer taps fall on odd samples, the middle one on an even.
  float out = 0;
  for (int j = 0; j < kPhase; ++j) out += h_[static_cast<std::size_t>(j)] * so[kPhase - 1 - j];
  out += 0.5f * se[kPhase - 1 - kMid];
  c.pos = (c.pos + 1) % kPhase;
  return out;
}

void Drive::setParam(int index, float v) {
  switch (index) {
    case kDrive: gain_ = std::pow(10.0f, v / 20.0f); break;
    case kShape:
      shape_ = std::clamp(static_cast<int>(std::lround(v)), 0, 2);
      l_.f1 = shapeAnti(shape_, l_.x1);  // the cached antiderivative belongs to the old shape
      r_.f1 = shapeAnti(shape_, r_.x1);
      break;
    case kBias: bias_ = v * 0.5f; break;
    case kTone: toneCoeff_ = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * v / kSr); break;
    case kLevel: level_ = std::pow(10.0f, v / 20.0f); break;
    case kMix: mix_ = v; break;
    default: break;
  }
}

void Drive::reset() {
  l_ = {};
  r_ = {};
}

float Drive::shapeOne(Channel& c, float in) const {
  const float x = in * gain_ + bias_;
  const float d = x - c.x1;
  const float f = shapeAnti(shape_, x);
  // First-order ADAA: the average of the shape between this sample and the last.
  const float y = std::fabs(d) > 1e-4f ? (f - c.f1) / d : shapeF(shape_, 0.5f * (x + c.x1));
  c.x1 = x;
  c.f1 = f;
  return y;
}

void Drive::process(float* left, float* right, std::size_t frames) {
  auto post = [this](Channel& c, float y) {
    // DC blocker (bias leaves an offset), then the tone low-pass.
    const float hp = y - c.dcIn + 0.9995f * c.dcOut;
    c.dcIn = y;
    c.dcOut = hp;
    c.tone += toneCoeff_ * (hp - c.tone);
    return c.tone * level_;
  };
  for (std::size_t i = 0; i < frames; ++i) {
    const float wl = post(l_, oversampled(l_, left[i])), wr = post(r_, oversampled(r_, right[i]));
    left[i] += mix_ * (wl - left[i]);
    right[i] += mix_ * (wr - right[i]);
  }
}
std::span<const ParamInfo> Delay::paramTable() { return kDelay; }
std::span<const ParamInfo> Reverb::paramTable() { return kReverb; }

// ---- compressor --------------------------------------------------------------------------

Compressor::Compressor() { applyDefaults(*this, kCompressor); }

void Compressor::setParam(int index, float v) {
  switch (index) {
    case kThreshold: threshold_ = v; break;
    case kRatio: ratio_ = std::max(1.0f, v); break;
    case kAttack: attackCoeff_ = msCoeff(v); break;
    case kRelease: releaseCoeff_ = msCoeff(v); break;
    case kKnee: knee_ = std::max(0.0f, v); break;
    case kMakeup: makeup_ = std::pow(10.0f, v / 20.0f); break;
    case kMix: mix_ = std::clamp(v, 0.0f, 1.0f); break;
    default: break;
  }
}

float Compressor::curve(float x) const {
  const float over = x - threshold_;
  if (2 * over < -knee_) return x;
  if (knee_ > 0 && 2 * std::fabs(over) <= knee_) {
    const float k = over + knee_ / 2;
    return x + (1 / ratio_ - 1) * k * k / (2 * knee_);
  }
  return threshold_ + over / ratio_;
}

void Compressor::process(float* left, float* right, std::size_t frames) {
  for (std::size_t i = 0; i < frames; ++i) {
    const float peak = std::max(std::fabs(left[i]), std::fabs(right[i]));
    const float x = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
    const float target = x - curve(x);
    const float c = target > gr_ ? attackCoeff_ : releaseCoeff_;
    gr_ = target + c * (gr_ - target);
    const float g = std::pow(10.0f, -gr_ / 20.0f) * makeup_;
    const float wet = 1 - mix_ + mix_ * g;
    left[i] *= wet;
    right[i] *= wet;
  }
}

// ---- chorus ------------------------------------------------------------------------------

Chorus::Chorus() : bufL_(kSize, 0.0f), bufR_(kSize, 0.0f) { applyDefaults(*this, kChorus); }

void Chorus::setParam(int index, float v) {
  switch (index) {
    case kRate: rate_ = v; break;
    case kDepth: depth_ = v; break;
    case kMix: mix_ = v; break;
    case kWidth: width_ = v; break;
    default: break;
  }
}

void Chorus::reset() {
  std::fill(bufL_.begin(), bufL_.end(), 0.0f);
  std::fill(bufR_.begin(), bufR_.end(), 0.0f);
}

void Chorus::process(float* left, float* right, std::size_t frames) {
  constexpr double kBase = 0.007 * kSampleRate;  // 7 ms
  const double depth = depth_ * 0.004 * kSampleRate;  // up to ±4 ms
  const double step = rate_ / kSampleRate;
  const double offset = width_ * 0.5;  // the right tap's LFO, up to half a cycle behind
  for (std::size_t i = 0; i < frames; ++i) {
    bufL_[static_cast<std::size_t>(write_)] = left[i];
    bufR_[static_cast<std::size_t>(write_)] = right[i];
    write_ = (write_ + 1) & (kSize - 1);
    const double dl = kBase + depth * std::sin(2 * std::numbers::pi * phase_);
    const double dr = kBase + depth * std::sin(2 * std::numbers::pi * (phase_ + offset));
    phase_ += step;
    if (phase_ >= 1) phase_ -= 1;
    const float wl = readRing(bufL_, kSize - 1, write_, dl);
    const float wr = readRing(bufR_, kSize - 1, write_, dr);
    left[i] += mix_ * (wl - left[i]);
    right[i] += mix_ * (wr - right[i]);
  }
}

// ---- delay -------------------------------------------------------------------------------

double Delay::divisionBeats(int d) {
  static const double beats[] = {0.125, 0.25 * 2 / 3, 0.25, 0.375, 0.5 * 2 / 3, 0.5, 0.75, 2.0 / 3, 1, 1.5, 2, 4};
  return beats[std::clamp(d, 0, 11)];
}

Delay::Delay() : bufL_(kSize, 0.0f), bufR_(kSize, 0.0f) {
  applyDefaults(*this, kDelay);
  updateFilters();
}

void Delay::setParam(int index, float v) {
  switch (index) {
    case kSync: sync_ = v >= 0.5f; break;
    case kDivision: division_ = static_cast<int>(std::lround(v)); break;
    case kTime: timeMs_ = v; break;
    case kFeedback: feedback_ = v; break;
    case kHighCut: highCut_ = v; updateFilters(); break;
    case kLowCut: lowCut_ = v; updateFilters(); break;
    case kPingPong: pingPong_ = v >= 0.5f; break;
    case kWidth: width_ = v; break;
    case kMix: mix_ = v; break;
    default: break;
  }
}

void Delay::updateFilters() {
  const auto lp = dsp::design(dsp::FilterShape::LowPass, highCut_, 0.707, 0, kSampleRate);
  const auto hp = dsp::design(dsp::FilterShape::HighPass, lowCut_, 0.707, 0, kSampleRate);
  lpL_.setCoeffs(lp);
  lpR_.setCoeffs(lp);
  hpL_.setCoeffs(hp);
  hpR_.setCoeffs(hp);
}

double Delay::targetSamples() const {
  const double s = sync_ ? divisionBeats(division_) * 60.0 / std::clamp(bpm_, 20.0, 400.0) * kSampleRate
                         : static_cast<double>(timeMs_) * kSampleRate / 1000.0;
  return std::clamp(s, 1.0, static_cast<double>(kSize - 8));
}

void Delay::reset() {
  std::fill(bufL_.begin(), bufL_.end(), 0.0f);
  std::fill(bufR_.begin(), bufR_.end(), 0.0f);
  lpL_.reset();
  lpR_.reset();
  hpL_.reset();
  hpR_.reset();
}

void Delay::process(float* left, float* right, std::size_t frames) {
  const double target = targetSamples();
  if (time_ < 0) time_ = target;
  constexpr double kGlide = 0.0004;  // ~50 ms: a tempo or time change bends, never clicks
  for (std::size_t i = 0; i < frames; ++i) {
    time_ += (target - time_) * kGlide;
    const float dl = lpL_.process(hpL_.process(readRing(bufL_, kSize - 1, write_, time_)));
    const float dr = lpR_.process(hpR_.process(readRing(bufR_, kSize - 1, write_, time_)));
    const float fl = softClip(dl * feedback_), fr = softClip(dr * feedback_);
    if (pingPong_) {
      bufL_[static_cast<std::size_t>(write_)] = 0.5f * (left[i] + right[i]) + fr;
      bufR_[static_cast<std::size_t>(write_)] = fl;
    } else {
      bufL_[static_cast<std::size_t>(write_)] = left[i] + fl;
      bufR_[static_cast<std::size_t>(write_)] = right[i] + fr;
    }
    write_ = (write_ + 1) & (kSize - 1);
    const float mid = 0.5f * (dl + dr), side = 0.5f * (dl - dr) * width_;
    const float wl = mid + side, wr = mid - side;
    if (wetOnly_) {
      left[i] = wl;
      right[i] = wr;
    } else {
      left[i] += mix_ * (wl - left[i]);
      right[i] += mix_ * (wr - right[i]);
    }
  }
}

// ---- reverb ------------------------------------------------------------------------------

namespace {
// Dattorro's figure 1 at 29761 Hz.
constexpr float kRate = static_cast<float>(kSampleRate) / 29761.0f;
constexpr float kDiffusers[4] = {142, 107, 379, 277};
// Per tank half: modulated allpass, delay, allpass, delay.
constexpr float kTank[8] = {672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
constexpr float kLoop = 672 + 4453 + 1800 + 3720 + 908 + 4217 + 2656 + 3163;
// Early reflections (Room): times in ms and gains, alternating sides.
constexpr float kEarlyMs[6] = {7.1f, 11.7f, 17.3f, 23.9f, 29.5f, 37.1f};
constexpr float kEarlyGain[6] = {0.8f, -0.7f, 0.6f, -0.5f, 0.42f, -0.35f};
struct Character {
  float scale, inDiff1, inDiff2, earlyLevel, modRate;
};
constexpr Character kCharacter[3] = {
    {1.0f, 0.75f, 0.625f, 0.0f, 0.9f},   // plate: the paper's values
    {1.5f, 0.80f, 0.70f, 0.15f, 0.6f},  // hall: bigger, denser
    {0.5f, 0.70f, 0.55f, 0.55f, 1.3f},  // room: small, with early reflections
};
}  // namespace

void Reverb::Line::init(int maxLength) {
  int n = 1;
  while (n < maxLength + 4) n <<= 1;
  buf.assign(static_cast<std::size_t>(n), 0.0f);
  mask = n - 1;
  write = 0;
}

float Reverb::Line::read(float delay) const {
  const float pos = static_cast<float>(write) - delay;
  const auto i = static_cast<int>(std::floor(pos));
  const float f = pos - static_cast<float>(i);
  const float a = buf[static_cast<std::size_t>(i & mask)], b = buf[static_cast<std::size_t>((i + 1) & mask)];
  return a + f * (b - a);
}

float Reverb::allpass(Line& l, float delay, float g, float x) {
  const float d = l.read(delay);
  const float v = x - g * d;
  l.push(v);
  return d + g * v;
}

Reverb::Reverb() {
  const float maxScale = 1.5f * 1.5f * kRate;  // hall at full size
  pre_.init(static_cast<int>(0.21f * kSr));
  early_.init(static_cast<int>(0.05f * kSr * 1.5f));
  for (int i = 0; i < 4; ++i) diffusers_[static_cast<std::size_t>(i)].init(static_cast<int>(kDiffusers[i] * maxScale));
  for (int i = 0; i < 8; ++i) tank_[static_cast<std::size_t>(i)].init(static_cast<int>(kTank[i] * maxScale + 64));
  applyDefaults(*this, kReverb);
  updateFilters();
}

void Reverb::setParam(int index, float v) {
  switch (index) {
    case kType: kind_ = std::clamp(static_cast<int>(std::lround(v)), 0, 2); break;
    case kSize: sizeParam_ = v; break;
    case kDecay: decayMs_ = v; break;
    case kPreDelay: preDelayMs_ = v; break;
    case kDamping: damping_ = v; break;
    case kWidth: width_ = v; break;
    case kMod: mod_ = v; break;
    case kLowCut: lowCut_ = v; updateFilters(); break;
    case kHighCut: highCut_ = v; updateFilters(); break;
    case kMix: mix_ = v; break;
    default: break;
  }
}

void Reverb::updateFilters() {
  const auto hp = dsp::design(dsp::FilterShape::HighPass, lowCut_, 0.707, 0, kSampleRate);
  const auto lp = dsp::design(dsp::FilterShape::LowPass, highCut_, 0.707, 0, kSampleRate);
  lcL_.setCoeffs(hp);
  lcR_.setCoeffs(hp);
  hcL_.setCoeffs(lp);
  hcR_.setCoeffs(lp);
}

int Reverb::memoryBlocks() const {
  // Pre-delay and early reflections, then one trip round the tank.
  const float samples = 0.2f * kSr + 0.06f * kSr + kLoop * kRate * std::max(size_, scale());
  return static_cast<int>(samples / 64) + 2;
}

float Reverb::scale() const { return kCharacter[kind_].scale * (0.5f + sizeParam_); }

void Reverb::reset() {
  for (Line* l : {&pre_, &early_}) std::fill(l->buf.begin(), l->buf.end(), 0.0f);
  for (Line& l : diffusers_) std::fill(l.buf.begin(), l.buf.end(), 0.0f);
  for (Line& l : tank_) std::fill(l.buf.begin(), l.buf.end(), 0.0f);
  tankFeedback_[0] = tankFeedback_[1] = damp_[0] = damp_[1] = bandwidth_ = 0;
  lcL_.reset();
  lcR_.reset();
  hcL_.reset();
  hcR_.reset();
}

void Reverb::process(float* left, float* right, std::size_t frames) {
  const Character& ch = kCharacter[kind_];
  if (size_ < 0) size_ = scale();
  if (preDelay_ < 0) preDelay_ = preDelayMs_;
  const float targetSize = scale(), targetPre = preDelayMs_;
  // Decay: RT60 from the loop length (4 decay gains per trip round the tank).
  const float loop = kLoop * kRate * targetSize;
  const float decay = std::min(0.9995f, std::pow(0.001f, loop / (4.0f * std::max(decayMs_, 50.0f) / 1000.0f * kSr)));
  const float damp = 0.05f + 0.65f * damping_;
  const float excursion = 12.0f * kRate * mod_;
  const double lfoStep = ch.modRate / kSampleRate;

  for (std::size_t i = 0; i < frames; ++i) {
    size_ += 0.0002f * (targetSize - size_);  // ~100 ms
    preDelay_ += 0.0005f * (targetPre - preDelay_);
    const float s = size_ * kRate;

    const float in = 0.5f * (left[i] + right[i]);
    pre_.push(in);
    float x = pre_.read(std::max(1.0f, preDelay_ * kSr / 1000.0f));
    early_.push(x);
    bandwidth_ += 0.9995f * (x - bandwidth_);
    x = bandwidth_;
    x = allpass(diffusers_[0], kDiffusers[0] * s, ch.inDiff1, x);
    x = allpass(diffusers_[1], kDiffusers[1] * s, ch.inDiff1, x);
    x = allpass(diffusers_[2], kDiffusers[2] * s, ch.inDiff2, x);
    x = allpass(diffusers_[3], kDiffusers[3] * s, ch.inDiff2, x);

    lfo_ += lfoStep;
    if (lfo_ >= 1) lfo_ -= 1;
    const auto phase = static_cast<float>(2 * std::numbers::pi * lfo_);
    const float mods[2] = {excursion * std::sin(phase), excursion * std::cos(phase)};

    // The two halves, each fed by the other's output from the last sample.
    float out[2];
    const float fb[2] = {tankFeedback_[1], tankFeedback_[0]};
    for (int h = 0; h < 2; ++h) {
      Line* t = &tank_[static_cast<std::size_t>(h * 4)];
      float y = allpass(t[0], kTank[h * 4] * s + mods[h] + excursion + 1, -0.7f, x + fb[h]);
      t[1].push(y);
      y = t[1].read(kTank[h * 4 + 1] * s);
      damp_[h] += (1 - damp) * (y - damp_[h]);
      y = damp_[h] * decay;
      y = allpass(t[2], kTank[h * 4 + 2] * s, 0.5f, y);
      t[3].push(y);
      out[h] = t[3].read(kTank[h * 4 + 3] * s) * decay;
    }
    tankFeedback_[0] = out[0];
    tankFeedback_[1] = out[1];

    // Output taps (Dattorro, table 2), scaled with the tank.
    auto tap = [&](int line, float at) { return tank_[static_cast<std::size_t>(line)].read(std::max(1.0f, at * s)); };
    float wl = tap(1, 266) + tap(1, 2974) - tap(2, 1913) + tap(3, 1996) - tap(5, 1990) - tap(6, 187) - tap(7, 1066);
    float wr = tap(5, 353) + tap(5, 3627) - tap(6, 1228) + tap(7, 2673) - tap(1, 2111) - tap(2, 335) - tap(3, 121);
    wl *= 0.6f;
    wr *= 0.6f;
    if (ch.earlyLevel > 0) {
      float el = 0, er = 0;
      for (int k = 0; k < 6; ++k) {
        const float v = kEarlyGain[k] * early_.read(std::max(1.0f, kEarlyMs[k] * kSr / 1000.0f * size_));
        (k % 2 ? er : el) += v;
      }
      wl += ch.earlyLevel * el;
      wr += ch.earlyLevel * er;
    }
    wl = hcL_.process(lcL_.process(wl));
    wr = hcR_.process(lcR_.process(wr));
    const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width_;
    wl = mid + side;
    wr = mid - side;
    if (wetOnly_) {
      left[i] = wl;
      right[i] = wr;
    } else {
      left[i] += mix_ * (wl - left[i]);
      right[i] += mix_ * (wr - right[i]);
    }
  }
  if (!std::isfinite(tankFeedback_[0] + tankFeedback_[1])) reset();
}

// ---- limiter -------------------------------------------------------------------------------

Limiter::Limiter() { reset(); }

void Limiter::reset() {
  delayL_.fill(0);
  delayR_.fill(0);
  boxes_.fill(1);
  boxSum_ = kLookahead;
  gain_ = 1;
  gr_ = 0;
  qHead_ = qSize_ = 0;
}

void Limiter::process(float* left, float* right, std::size_t frames) {
  // Gain never falls faster than needed and comes back over ~100 ms.
  static const float release = 1.0f - std::exp(-1.0f / (0.1f * kSr));
  constexpr int kWindow = kLookahead + 1;  // the min covers one more sample than the box (see header)
  constexpr int kCap = static_cast<int>(std::tuple_size_v<decltype(queue_)>);
  for (std::size_t i = 0; i < frames; ++i) {
    const float peak = std::max(std::fabs(left[i]), std::fabs(right[i]));
    const float target = peak > kCeiling ? kCeiling / peak : 1.0f;
    // Sliding minimum of the targets over the last kWindow samples.
    while (qSize_ > 0 && queue_[static_cast<std::size_t>((qHead_ + qSize_ - 1) % kCap)].first >= target) --qSize_;
    queue_[static_cast<std::size_t>((qHead_ + qSize_) % kCap)] = {target, t_};
    ++qSize_;
    while (queue_[static_cast<std::size_t>(qHead_)].second <= t_ - kWindow) {
      qHead_ = (qHead_ + 1) % kCap;
      --qSize_;
    }
    const float held = queue_[static_cast<std::size_t>(qHead_)].first;
    // …smoothed by a moving average as long as the lookahead, so the gain
    // ramps down ahead of the peak instead of stepping.
    boxSum_ += held - boxes_[static_cast<std::size_t>(pos_)];
    boxes_[static_cast<std::size_t>(pos_)] = held;
    const auto box = static_cast<float>(boxSum_ / kLookahead);
    gain_ = box < gain_ ? box : gain_ + (box - gain_) * release;
    gain_ = std::min(gain_, box);

    const float dl = delayL_[static_cast<std::size_t>(pos_)], dr = delayR_[static_cast<std::size_t>(pos_)];
    delayL_[static_cast<std::size_t>(pos_)] = left[i];
    delayR_[static_cast<std::size_t>(pos_)] = right[i];
    pos_ = (pos_ + 1) % kLookahead;
    ++t_;
    if (t_ > (1 << 30)) {  // keep the indices small; shift the queue with them
      for (int k = 0; k < qSize_; ++k) queue_[static_cast<std::size_t>((qHead_ + k) % kCap)].second -= 1 << 29;
      t_ -= 1 << 29;
    }
    left[i] = std::clamp(dl * gain_, -kCeiling, kCeiling);
    right[i] = std::clamp(dr * gain_, -kCeiling, kCeiling);
  }
  gr_ = gain_ < 1 ? -20.0f * std::log10(gain_) : 0.0f;
}

}  // namespace ion
