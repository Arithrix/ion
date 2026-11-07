#pragma once

// The effects themselves. Each class has its parameter enum and table; the
// table order is the index used by setParam and in project files (by key).

#include <array>
#include <vector>

#include "dsp/biquad.h"
#include "engine/fx/effect.h"

namespace ion {

// Feed-forward compressor (HANDOFF §7.2): stereo-linked peak detector, soft
// knee gain computer and log-domain attack/release smoothing, after Giannoulis,
// Massberg & Reiss, "Digital Dynamic Range Compressor Design" (JAES 2012).
class Compressor final : public Effect {
 public:
  enum Param : int { kThreshold, kRatio, kAttack, kRelease, kKnee, kMakeup, kMix, kParamCount };
  static std::span<const ParamInfo> paramTable();
  Compressor();
  EffectType type() const override { return EffectType::Compressor; }
  void setParam(int index, float value) override;
  void process(float* left, float* right, std::size_t frames) override;
  void reset() override { gr_ = 0; }
  float reduction() const override { return gr_; }
  // The static curve: output level in dB for an input level in dB (tests, UI).
  float curve(float inDb) const;

 private:
  float threshold_ = -18, ratio_ = 3, knee_ = 6, makeup_ = 1, mix_ = 1;
  float attackCoeff_ = 0, releaseCoeff_ = 0;
  float gr_ = 0;  // smoothed gain reduction, dB
};

// Stereo chorus (HANDOFF §7.2): two modulated delay taps, the right one's LFO
// offset by up to half a cycle (Width), 4-point Hermite reads.
class Chorus final : public Effect {
 public:
  enum Param : int { kRate, kDepth, kMix, kWidth, kParamCount };
  static std::span<const ParamInfo> paramTable();
  Chorus();
  EffectType type() const override { return EffectType::Chorus; }
  void setParam(int index, float value) override;
  void process(float* left, float* right, std::size_t frames) override;
  void reset() override;

 private:
  static constexpr int kSize = 4096;  // 85 ms, a power of two
  std::vector<float> bufL_, bufR_;
  int write_ = 0;
  double phase_ = 0;
  float rate_ = 0.5f, depth_ = 0.5f, mix_ = 0.5f, width_ = 0.8f;
};

// Tempo-synced stereo delay (HANDOFF §7.3 A): straight, dotted and triplet
// divisions or milliseconds; a high- and low-cut filter and soft saturation in
// the feedback path; ping-pong; width. Time changes glide rather than jump.
class Delay final : public Effect {
 public:
  enum Param : int { kSync, kDivision, kTime, kFeedback, kHighCut, kLowCut, kPingPong, kWidth, kMix, kParamCount };
  static std::span<const ParamInfo> paramTable();
  static double divisionBeats(int division);  // a quarter note = 1
  static constexpr int kSize = 1 << 18;        // 5.46 s at 48 kHz
  Delay();
  EffectType type() const override { return EffectType::Delay; }
  void setParam(int index, float value) override;
  void process(float* left, float* right, std::size_t frames) override;
  void reset() override;
  void setWetOnly(bool on) override { wetOnly_ = on; }
  void setTempo(double bpm) override { bpm_ = bpm; }
  double targetSamples() const;  // the delay time it's heading for (tests)

 private:
  void updateFilters();
  std::vector<float> bufL_, bufR_;
  int write_ = 0;
  double time_ = -1;  // current delay in samples; glides to targetSamples()
  double bpm_ = 120;
  bool sync_ = true, pingPong_ = false, wetOnly_ = false;
  int division_ = 6;
  float timeMs_ = 375, feedback_ = 0.4f, highCut_ = 8000, lowCut_ = 80, width_ = 1, mix_ = 0.3f;
  dsp::Biquad lpL_, lpR_, hpL_, hpR_;
};

// Reverb (HANDOFF §7.3 B): Dattorro's plate tank ("Effect Design Part 1",
// JAES 1997) with three characters. Plate is the paper's figure; Hall scales
// the tank up and diffuses more; Room scales it down and adds early
// reflections. Size and pre-delay glide, so they can be swept.
class Reverb final : public Effect {
 public:
  enum Param : int { kType, kSize, kDecay, kPreDelay, kDamping, kWidth, kMod, kLowCut, kHighCut, kMix, kParamCount };
  enum Kind : int { kPlate, kHall, kRoom };
  static std::span<const ParamInfo> paramTable();
  Reverb();
  EffectType type() const override { return EffectType::Reverb; }
  void setParam(int index, float value) override;
  void process(float* left, float* right, std::size_t frames) override;
  void reset() override;
  void setWetOnly(bool on) override { wetOnly_ = on; }

 private:
  struct Line {  // a delay line read with linear interpolation
    std::vector<float> buf;
    int mask = 0, write = 0;
    void init(int maxLength);
    void push(float x) { buf[static_cast<std::size_t>(write)] = x; write = (write + 1) & mask; }
    float read(float delay) const;  // samples ago, ≥ 1
  };
  float allpass(Line& l, float delay, float g, float x);
  void updateFilters();

  float scale() const;  // tank size factor for the current kind and size
  Line pre_, early_;
  std::array<Line, 4> diffusers_;
  std::array<Line, 8> tank_;  // per half: mod allpass, delay, allpass, delay
  float tankFeedback_[2] = {0, 0};
  float damp_[2] = {0, 0};
  float bandwidth_ = 0;
  double lfo_ = 0;
  float size_ = -1, preDelay_ = -1;  // smoothed
  int kind_ = kPlate;
  float sizeParam_ = 0.6f, decayMs_ = 2400, preDelayMs_ = 20, damping_ = 0.5f, width_ = 0.9f, mod_ = 0.25f;
  float lowCut_ = 200, highCut_ = 9000, mix_ = 0.25f;
  bool wetOnly_ = false;
  dsp::Biquad lcL_, lcR_, hcL_, hcR_;
};

// The master's safety limiter (HANDOFF §7.4): 1 ms lookahead, so peaks are
// caught before they happen rather than clipped; the output never exceeds the
// ceiling (−0.3 dBFS). Not a user effect: always last on the master. The
// lookahead is latency on everything you hear, so it's kept short (D-45).
class Limiter {
 public:
  static constexpr int kLookahead = 48;  // 1 ms
  static constexpr float kCeiling = 0.9660509f;
  Limiter();
  void process(float* left, float* right, std::size_t frames);
  void reset();
  float reduction() const { return gr_; }  // dB, ≥ 0

 private:
  std::array<float, kLookahead> delayL_{}, delayR_{}, targets_{}, boxes_{};
  int pos_ = 0;
  double boxSum_ = kLookahead;
  float gain_ = 1, gr_ = 0;
  // The minimum of the last kLookahead targets, as a monotonic queue.
  std::array<std::pair<float, int>, kLookahead + 2> queue_{};
  int qHead_ = 0, qSize_ = 0, t_ = 0;
};

}  // namespace ion
