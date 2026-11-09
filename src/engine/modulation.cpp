#include "engine/modulation.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/engine.h"
#include "engine/envelope_params.h"

namespace ion {

namespace {

constexpr int kLfoBase = 0;
constexpr int kEnvBase = kLfos * kLfoParamCount;

float lfoParam(const float* mod, int lfo, int p) { return mod[kLfoBase + lfo * kLfoParamCount + p]; }
float envParam(const float* mod, int env, int p) { return mod[kEnvBase + env * kModEnvParamCount + p]; }

double frac(double x) { return x - std::floor(x); }

}  // namespace

float TrackModulation::random() noexcept {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<float>(rng_ >> 8) / 8388608.0f - 1.0f;  // −1…1
}

void TrackModulation::newStep(Lfo& l) noexcept {
  l.from = l.to;
  l.to = random();
}

float TrackModulation::shape(int s, double p, const Lfo& l) noexcept {
  switch (s) {
    case kLfoSine: return static_cast<float>(std::sin(2.0 * std::numbers::pi * p));
    case kLfoTriangle: return static_cast<float>(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4);
    case kLfoSawUp: return static_cast<float>(2 * p - 1);
    case kLfoSawDown: return static_cast<float>(1 - 2 * p);
    case kLfoSquare: return p < 0.5 ? 1.0f : -1.0f;
    case kLfoSampleHold: return l.to;
    case kLfoSmooth: {
      const float t = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * p));
      return l.from + (l.to - l.from) * t;
    }
    default: return 0;
  }
}

dsp::Ahdsr::Coeffs TrackModulation::coeffs(const float* mod, int env) const noexcept {
  return envelopeCoeffs(mod + kEnvBase + env * kModEnvParamCount);
}

void TrackModulation::advance(const float* mod, double bpm, std::size_t frames) noexcept {
  if (bpm <= 0) bpm = 120;
  for (int i = 0; i < kLfos; ++i) {
    Lfo& l = lfo_[static_cast<std::size_t>(i)];
    const bool sync = lfoParam(mod, i, kLfoSync) >= 0.5f;
    const double hz = sync ? bpm / 60.0 / lfoDivisionBeats(static_cast<int>(std::lround(lfoParam(mod, i, kLfoDivision))))
                           : static_cast<double>(lfoParam(mod, i, kLfoRate));
    const double next = l.phase + hz * static_cast<double>(frames) / kSampleRate;
    if (next >= 1.0) newStep(l);  // a new cycle: S&H and smooth pick their next value
    l.phase = frac(next);
    const double p = frac(l.phase + static_cast<double>(lfoParam(mod, i, kLfoPhase)) / 360.0);
    l.value = shape(static_cast<int>(std::lround(lfoParam(mod, i, kLfoShape))), p, l);
  }
  for (int i = 0; i < kModEnvs; ++i) {
    dsp::Ahdsr& e = env_[static_cast<std::size_t>(i)];
    if (e.idle()) continue;
    const dsp::Ahdsr::Coeffs c = coeffs(mod, i);
    for (std::size_t f = 0; f < frames; ++f) e.next(c);
  }
}

void TrackModulation::noteOn(const float* mod, uint8_t note, uint8_t velocity) noexcept {
  const bool first = heldCount_ == 0;
  auto& word = held_[note >> 5];
  const uint32_t bit = 1u << (note & 31);
  if (!(word & bit)) {
    word |= bit;
    ++heldCount_;
  }
  velocity_ = static_cast<float>(velocity) / 127.0f;
  key_ = static_cast<float>(note) / 127.0f;
  random_ = random();
  for (int i = 0; i < kModEnvs; ++i) {
    const bool legato = std::lround(envParam(mod, i, kMEnvTrigger)) == 1;
    if (legato && !first) continue;
    dsp::Ahdsr& e = env_[static_cast<std::size_t>(i)];
    e.start();
    e.next(coeffs(mod, i));  // so an instant attack is already at the top for the note's first sample
  }
  for (int i = 0; i < kLfos; ++i)
    if (std::lround(lfoParam(mod, i, kLfoRestart)) == kRestartNote) {
      Lfo& l = lfo_[static_cast<std::size_t>(i)];
      l.phase = 0;
      newStep(l);
      l.value = shape(static_cast<int>(std::lround(lfoParam(mod, i, kLfoShape))), frac(lfoParam(mod, i, kLfoPhase) / 360.0), l);
    }
}

void TrackModulation::noteOff(uint8_t note) noexcept {
  auto& word = held_[note >> 5];
  const uint32_t bit = 1u << (note & 31);
  if (!(word & bit)) return;
  word &= ~bit;
  if (--heldCount_ == 0)
    for (dsp::Ahdsr& e : env_) e.release();
}

void TrackModulation::downbeat(const float* mod, int64_t bar) noexcept {
  for (int i = 0; i < kLfos; ++i) {
    Lfo& l = lfo_[static_cast<std::size_t>(i)];
    const int restart = static_cast<int>(std::lround(lfoParam(mod, i, kLfoRestart)));
    if (restart == kRestartBar) {
      l.phase = 0;
      newStep(l);
    } else if (lfoParam(mod, i, kLfoSync) >= 0.5f && restart == kRestartFree) {
      // Synced and free-running: line the phase up with the bar, so it stays
      // locked to the OXI however long it runs (a tiny correction each bar).
      const double beats = lfoDivisionBeats(static_cast<int>(std::lround(lfoParam(mod, i, kLfoDivision))));
      l.phase = frac(static_cast<double>(bar) * 4.0 / beats);
    }
  }
}

void TrackModulation::reset() noexcept {
  *this = TrackModulation{};
}

float TrackModulation::source(int s) const noexcept {
  switch (s) {
    case kSrcLfo1: return lfo_[0].value;
    case kSrcLfo2: return lfo_[1].value;
    case kSrcEnv1: return env_[0].level();
    case kSrcEnv2: return env_[1].level();
    case kSrcVelocity: return velocity_;
    case kSrcModWheel: return wheel_;
    case kSrcAftertouch: return touch_;
    case kSrcKey: return key_;
    case kSrcRandom: return random_;
    default: return 0;
  }
}

}  // namespace ion
