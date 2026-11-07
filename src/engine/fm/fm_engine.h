#pragma once

#include <memory>

#include "engine/engine.h"
#include "library/syx.h"

namespace ion {

class DexedCore;

// DX7-compatible 6-operator FM, built on Synth_Dexed.
class FmEngine final : public Engine {
 public:
  // Allocates all voices up front; construct off the audio thread.
  explicit FmEngine(int polyphony = 8);
  ~FmEngine() override;

  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t controller, uint8_t value) override;
  void pitchBend(int value) override;
  void allNotesOff() override;
  void render(float* left, float* right) override;
  int voiceCount() const override { return voices_; }

  // Real-time safe. Synth_Dexed stops held notes when a voice loads (it calls
  // panic()); Phase 2 deals with that for live browsing.
  void loadVoice(const syx::Voice& voice);

  // Sets the millisecond clock Synth_Dexed uses to pick voices to steal, from
  // the module's sample counter. Audio thread, once per block.
  static void setClock(uint64_t frames);

 private:
  std::unique_ptr<DexedCore> dexed_;
  float mono_[kBlockSize]{};
  int voices_ = 0;
};

}  // namespace ion
