#pragma once

#include <array>
#include <memory>
#include <span>

#include "dsp/ahdsr.h"
#include "engine/engine.h"

namespace rosic {
class Open303;
}

namespace ion {

// A 303-style bass (PLAN Phase 9, DECISIONS D-60), on Robin Schmidt's Open303
// (MIT). Mono. Overlapping notes slide; notes at or above the accent
// threshold (velocity 100, the 303 convention) are accented. The OXI is the
// sequencer: ion plays what it's sent.
//
// After the voice comes the uniform amp envelope (D-50), wide open by
// default, as on FM and Plaits.
class AcidEngine final : public Engine {
 public:
  enum Param : int {
    kWaveform,   // saw ↔ square
    kCutoff, kResonance, kEnvMod, kDecay, kAccent,
    kSlide,      // ms
    kAccentAt,   // velocity at which a note is accented
    kTune, kFine, kBendRange, kLevel,
    kAmpAttack, kAmpHold, kAmpDecay, kAmpSustain, kAmpRelease, kVelAmp,
    kParamCount
  };
  static constexpr int kFirstAmp = kAmpAttack;
  static std::span<const ParamInfo> paramTable();

  AcidEngine();  // allocates (Open303's wavetables): off the audio thread
  ~AcidEngine() override;

  EngineType type() const override { return EngineType::Acid; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t, uint8_t) override {}
  void pitchBend(int value) override;
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override { return awake_ ? 1 : 0; }

  bool awake() const { return awake_; }  // for tests: rendering, or asleep after silence

 private:
  void updateTuning();

  std::unique_ptr<rosic::Open303> v_;
  std::array<float, kParamCount> values_{};
  dsp::Ahdsr amp_;
  dsp::Ahdsr::Coeffs ampCoeffs_;
  std::array<uint32_t, 4> held_{};  // keys down, 128 bits
  int heldCount_ = 0;
  float velGain_ = 1;
  float bend_ = 0;        // −1…1
  bool awake_ = false;    // Open303 never idles by itself; ion stops calling it once it's silent
  int quiet_ = 0;         // samples in a row below −90 dB with no key down
};

}  // namespace ion
