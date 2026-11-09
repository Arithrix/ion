#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "dsp/ahdsr.h"
#include "dsp/svf.h"
#include "engine/engine.h"

namespace ion {

// CHIP (PLAN Phase 10, DECISIONS D-61): ion's own lo-fi wave synth, in the
// spirit of the M8's WavSynth. Each note reads a small wave table (4–256
// steps, 1–8 bits) built from a basic shape and reshaped: repeated (mult),
// pushed to one side (warp), mirrored, and driven into a limiter (clip, sine,
// fold, wrap). The table is read without interpolation, so it steps and
// aliases the way chip sound does. Then a filter, the uniform amp envelope,
// and an optional lower sample rate for the whole track.
//
// The table is rebuilt only when a setting that shapes it changes (at most
// once a block), and shared by every voice.
class ChipEngine final : public Engine {
 public:
  enum Param : int {
    kShape, kSize, kMult, kWarp, kMirror, kAmp, kLimit, kBits,
    kFilter, kCutoff, kResonance,
    kRate,  // the track's sample rate, for the crunch
    kTune, kFine, kGlide, kPoly, kBendRange, kLevel,
    kAmpAttack, kAmpHold, kAmpDecay, kAmpSustain, kAmpRelease, kVelAmp,
    kParamCount
  };
  enum Shape : int { kPulse12, kPulse25, kPulse50, kPulse75, kSaw, kTriangle, kSine, kNoiseTone, kNoise, kShapeCount };
  enum Limit : int { kClip, kSineLimit, kFold, kWrap };
  enum Filter : int { kFilterOff, kLowPass, kHighPass, kBandPass, kNotch };
  static constexpr int kFirstAmp = kAmpAttack;
  static constexpr int kMaxVoices = 8;
  static constexpr int kMaxSize = 256;
  static std::span<const ParamInfo> paramTable();

  ChipEngine();

  EngineType type() const override { return EngineType::Chip; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t, uint8_t) override {}
  void pitchBend(int value) override { bend_ = static_cast<float>(value) / 8192.0f; }
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override;

  // The table as it is now (tests, and the screen draws it).
  std::span<const float> table() const { return {table_.data(), static_cast<std::size_t>(size_)}; }
  void rebuildIfNeeded();  // audio thread (render calls it); public for tests and the UI's drawing copy

 private:
  struct Voice {
    dsp::Ahdsr amp;
    dsp::Svf svf;
    double phase = 0;
    float pitch = 0, glideFrom = 0;
    int glideLeft = 0;
    uint8_t note = 0;
    bool gate = false, on = false;
    int cut = -1;          // a 5 ms fade that ends the note when nothing else would
    float velGain = 1;
    uint32_t age = 0;
    uint16_t lfsr = 1;     // the noise shapes' shift register
    double noisePhase = 0;
    float noiseOut = 1;
    bool active() const { return on; }
    bool released() const { return !gate; }
  };
  float shapeAt(double p) const;
  float limitAndQuantize(float v) const;
  void start(Voice& v, uint8_t note, uint8_t velocity);

  std::array<float, kParamCount> values_{};
  std::array<float, kMaxSize> table_{};
  int size_ = 32;
  bool dirty_ = true;
  float noiseHigh_ = 1, noiseLow_ = -1;  // the noise shapes' two levels, through amp, limiter and bits
  std::array<Voice, kMaxVoices> voices_{};
  dsp::Ahdsr::Coeffs ampCoeffs_;
  dsp::SvfCoeffs svfCoeffs_;
  uint32_t age_ = 0;
  float bend_ = 0;
  float lastPitch_ = -1;
  // The track-wide sample-rate reduction: a sample-and-hold.
  double holdPhase_ = 1;
  float holdL_ = 0;
};

}  // namespace ion
