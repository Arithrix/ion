#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "dsp/ahdsr.h"
#include "engine/engine.h"
#include "engine/sample.h"
#include "engine/sample_retirement.h"

namespace ion {

// The sampler (HANDOFF §6.2): one sample per track, played chromatically
// from a root note across a key range. Loop off, forward or ping-pong (with
// an optional crossfade at the loop point); amp and filter ADSR; a state-
// variable filter (LP12, LP24, HP, BP) with key tracking; one LFO, free or
// tempo-synced, onto pitch, cutoff, pan and level; glide; velocity to amp
// and filter. Playback mode is Repitch; Stretch is reserved for later and
// already has its place in the parameters and in project files.
//
// Sample points (start, loop start, loop end) are fractions of the sample's
// length, so a project keeps them when the file is collected or resampled.
class SamplerEngine final : public Engine {
 public:
  enum Param : int {
    // Play
    kMode, kRoot, kLowKey, kHighKey, kTune, kFine, kLevel, kStart, kLoop, kLoopStart, kLoopEnd, kCrossfade, kSnap,
    kGlide, kPoly, kBendRange,
    // Amp: the uniform envelope (D-50), then velocity
    kAmpAttack, kAmpHold, kAmpDecay, kAmpSustain, kAmpRelease, kVelAmp,
    // Filter, with the same envelope
    kFilter, kCutoff, kReso, kKeyTrack, kEnvAmount, kFAttack, kFHold, kFDecay, kFSustain, kFRelease, kVelFilter,
    // Mod
    kLfoShape, kLfoSync, kLfoRate, kLfoDivision, kLfoPitch, kLfoCutoff, kLfoPan, kLfoLevel,
    kParamCount
  };
  enum Loop : int { kLoopOff, kLoopForward, kLoopPingPong };
  enum Filter : int { kFilterOff, kLp12, kLp24, kHp, kBp };
  enum Shape : int { kSine, kTriangle, kSquare, kSampleHold };
  static std::span<const ParamInfo> paramTable();
  static double divisionBeats(int division);  // the LFO's synced period, quarter note = 1

  static constexpr int kMaxPolyphony = 16;
  static constexpr int kVoices = kMaxPolyphony + 8;  // spare voices to fade out stolen ones
  static constexpr int kFadeFrames = 240;            // 5 ms

  SamplerEngine();

  EngineType type() const override { return EngineType::Sampler; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t controller, uint8_t value) override;
  void pitchBend(int value) override { bend_ = static_cast<float>(value) / 8192.0f; }
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override { return voices_; }
  void setTempo(double bpm) override { bpm_ = bpm; }

  void setSample(int slot, const Sample* s) override;
  bool popReleased(const Sample*& s) override { return retirement_.pop(s); }
  void heldSamples(std::vector<const Sample*>& out) const override;

  const Sample* sample() const { return sample_; }
  float value(int p) const { return v_[static_cast<std::size_t>(p)]; }
  float lfo() const { return lfoValue_; }  // −1…1, for tests and the UI

 private:
  struct Voice {
    bool on = false;
    const Sample* sample = nullptr;
    uint8_t note = 0;
    float velocity = 1;
    uint32_t age = 0;
    bool held = true;      // key down (or held by the sustain pedal)
    bool keyDown = true;
    bool fading = false;
    float fade = 1;
    double pos = 0;
    int direction = 1;
    float pitch = 0;       // semitones, gliding to `note`
    dsp::Ahdsr amp, filt;
    float s1[2][2]{}, s2[2][2]{};  // SVF state [stage][channel]
    bool active() const { return on; }
    bool released() const { return !held || fading; }
  };

  void renderVoice(Voice& v, float* left, float* right, std::size_t frames, float lfo);
  bool inUse(const Sample* s) const;
  void releaseVoice(Voice& v);

  std::array<float, kParamCount> v_{};
  const Sample* sample_ = nullptr;
  std::array<Voice, kVoices> voice_{};
  SampleRetirement<32> retirement_;
  uint32_t age_ = 0;
  int voices_ = 0;
  float lastPitch_ = -1;  // the last note played, for glide
  bool sustain_ = false;
  float bend_ = 0;
  double bpm_ = 120;
  double lfoPhase_ = 0;
  float lfoValue_ = 0, lfoHold_ = 0;
  uint32_t rng_ = 22222;
};

}  // namespace ion
