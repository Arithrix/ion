#pragma once

#include <memory>
#include <span>

#include "engine/engine.h"
#include "library/syx.h"

namespace ion {

class DexedCore;

// DX7-compatible 6-operator FM, built on Synth_Dexed.
class FmEngine final : public Engine {
 public:
  enum Param : int { kPoly, kBendRange, kMono, kParamCount };
  static std::span<const ParamInfo> paramTable();
  static constexpr int kMaxPolyphony = 16;

  // Allocates every voice up front; construct off the audio thread.
  FmEngine();
  ~FmEngine() override;

  EngineType type() const override { return EngineType::Fm; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t controller, uint8_t value) override;
  void pitchBend(int value) override;
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override { return voices_; }

  // Real-time safe. Synth_Dexed stops held notes when a voice loads (it calls
  // panic()); Phase 2 deals with that for live browsing.
  void loadVoice(const syx::Voice& voice);

  // Sets the millisecond clock Synth_Dexed uses to pick voices to steal, from
  // the module's sample counter. Audio thread, once per block.
  static void setClock(uint64_t frames);

 private:
  std::unique_ptr<DexedCore> dexed_;
  // Synth_Dexed renders 64 frames at a time; render() hands them out in the
  // sizes the module asks for, so note-ons land on Dexed's 64-frame grid.
  float mono_[kBlockSize]{};
  std::size_t pos_ = kBlockSize;
  int voices_ = 0;
};

}  // namespace ion
