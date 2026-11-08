#pragma once

// The envelope every instrument uses, for amp and filter (DECISIONS D-50):
// attack (linear), hold, decay and release (exponential, −60 dB in the time
// set), sustain. Header-only and dependency-free: Synth_Dexed uses it too.

#include <cmath>
#include <cstdint>

namespace ion::dsp {

class Ahdsr {
 public:
  enum class Stage : uint8_t { Idle, Attack, Hold, Decay, Sustain, Release };

  // Per-sample values, computed from the settings once per block.
  struct Coeffs {
    float attackStep = 1;  // per sample; 1 = instant
    int holdFrames = 0;    // −1: hold until released ("full")
    float decay = 0;       // per-sample coefficient toward sustain
    float sustain = 1;
    float release = 0;     // per-sample coefficient toward 0; 1 = no fade ("off")
  };
  // Times in ms. holdFull/releaseOff are the "full" and "off" ends of those ranges.
  static Coeffs make(float attackMs, float holdMs, bool holdFull, float decayMs, float sustain, float releaseMs, bool releaseOff,
                     float sampleRate = 48000.0f) {
    Coeffs c;
    const float perMs = sampleRate / 1000.0f;
    c.attackStep = attackMs * perMs <= 1.0f ? 1.0f : 1.0f / (attackMs * perMs);
    c.holdFrames = holdFull ? -1 : static_cast<int>(holdMs * perMs);
    c.decay = falloff(decayMs * perMs);
    c.sustain = sustain < 0 ? 0 : sustain > 1 ? 1 : sustain;
    c.release = releaseOff ? 1.0f : falloff(releaseMs * perMs);
    return c;
  }

  void start() {
    stage_ = Stage::Attack;
    holdLeft_ = -2;
  }
  void release() {
    if (stage_ != Stage::Idle) stage_ = Stage::Release;
  }
  void reset() {
    stage_ = Stage::Idle;
    level_ = 0;
  }
  Stage stage() const { return stage_; }
  bool idle() const { return stage_ == Stage::Idle; }
  float level() const { return level_; }
  // True while the level can't change (held, sustaining at the set level, or
  // a release that's "off"): the caller may use level() for a whole block.
  bool steady(const Coeffs& c) const {
    switch (stage_) {
      case Stage::Hold: return holdLeft_ == -1;
      case Stage::Sustain: return level_ == c.sustain;
      case Stage::Release: return c.release >= 1.0f;
      default: return false;
    }
  }

  float next(const Coeffs& c) {
    switch (stage_) {
      case Stage::Idle: level_ = 0; break;
      case Stage::Attack:
        level_ += c.attackStep;
        if (level_ >= 1) {
          level_ = 1;
          stage_ = Stage::Hold;
          holdLeft_ = c.holdFrames;
        }
        break;
      case Stage::Hold:
        if (holdLeft_ >= 0 && holdLeft_-- == 0) stage_ = Stage::Decay;  // −1: held until released
        break;
      case Stage::Decay:
        level_ = c.sustain + (level_ - c.sustain) * c.decay;
        if (level_ - c.sustain < 1e-4f) stage_ = c.sustain < 1e-4f ? Stage::Idle : Stage::Sustain;
        break;
      case Stage::Sustain:
        level_ += (c.sustain - level_) * 0.002f;  // follows the knob without a step
        if (std::fabs(level_ - c.sustain) < 1e-6f) level_ = c.sustain;
        if (c.sustain < 1e-4f && level_ < 1e-4f) stage_ = Stage::Idle;
        break;
      case Stage::Release:
        if (c.release < 1.0f) {
          level_ *= c.release;
          if (level_ < 1e-4f) stage_ = Stage::Idle;
        }
        break;
    }
    if (stage_ == Stage::Idle) level_ = 0;
    return level_;
  }

 private:
  // The coefficient that takes a level down 60 dB in `samples`.
  static float falloff(float samples) { return samples < 1 ? 0.0f : std::exp(-6.9077553f / samples); }

  Stage stage_ = Stage::Idle;
  float level_ = 0;
  int holdLeft_ = 0;
};

}  // namespace ion::dsp
