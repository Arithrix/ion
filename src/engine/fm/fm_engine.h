#pragma once

#include <array>
#include <memory>
#include <span>

#include "engine/engine.h"
#include "engine/fm/dx7_voice.h"
#include "library/syx.h"

namespace ion {

class DexedCore;

// DX7-compatible 6-operator FM, built on Synth_Dexed.
//
// Changing patch never cuts a note (HANDOFF §6.3: browsing loads patches while
// the OXI plays). The engine keeps three Synth_Dexed instances:
//   active — gets new notes, plays the current patch;
//   tail   — the previous patch, finishing notes that were held when it changed;
//   spare  — silent, ready for the next patch.
// A patch loads into the spare, which becomes active. If the old tail is still
// sounding at that point it fades out over 10 ms, so even fast scrolling
// through the library doesn't click. Only instances with sounding notes are
// rendered, so the extra instances cost memory, not CPU.
//
// After the patch, each voice goes through the uniform amp envelope (D-50).
// Its defaults are wide open (no attack, sustain 100%, release off), so a
// patch sounds as stored until the envelope is used to shape it.
class FmEngine final : public Engine {
 public:
  enum Param : int {
    kPoly, kBendRange, kMono,
    kBright, kFeedback, kDetune, kVelocity,  // the macros, in dx7::kMacroNames order
    kAmpAttack, kAmpHold, kAmpDecay, kAmpSustain, kAmpRelease, kVelAmp,  // the uniform amp envelope (D-50)
    kParamCount
  };
  static constexpr int kFirstMacro = kBright;
  static constexpr int kFirstAmp = kAmpAttack;
  static std::span<const ParamInfo> paramTable();
  static constexpr int kMaxPolyphony = 16;
  static constexpr int kSlots = 3;
  static constexpr int kFadeFrames = 480;  // 10 ms
  // Synth_Dexed's DC blocker (time constant 8 ms) keeps ringing after the last
  // voice ends; an instance renders this long more before it goes idle.
  static constexpr int kQuietFrames = 4800;

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

  // Real-time safe. Notes already sounding finish with the old patch.
  void loadVoice(const syx::Voice& voice);

  const dx7::Macros& macros() const { return macros_; }
  // The voice new notes play: the loaded patch with the macros applied.
  syx::Voice effectiveVoice() const;
  int soundingSlots() const;  // for tests

  // Sets the millisecond clock Synth_Dexed uses to pick voices to steal, from
  // the module's sample counter. Audio thread, once per block.
  static void setClock(uint64_t frames);

 private:
  struct Slot {
    std::unique_ptr<DexedCore> dexed;
    syx::Voice patch{};    // as loaded, without macros
    bool sounding = false; // has (or may have) live notes: rendered
    int fade = 0;          // frames left in a fade-out; 0 = not fading
    int quiet = 0;         // frames rendered since the last note died away
  };

  void each(auto&& fn);
  void applyMacros(Slot& s);
  void syncControllers(Slot& s);
  void stop(Slot& s);

  std::array<Slot, kSlots> slots_;
  int active_ = 0;
  int tail_ = -1;
  std::array<int8_t, 128> owner_{};  // slot that played each held note, or -1
  dx7::Macros macros_{};
  std::array<float, 6> amp_{};  // the amp envelope's settings, then vel→amp
  void applyAmp();

  // Controller state, so a newly active instance starts where the others are.
  int pitchBend_ = 0;
  uint8_t modWheel_ = 0, breath_ = 0, foot_ = 0;
  bool sustain_ = false;
  bool mono_ = false;

  // Synth_Dexed renders 64 frames at a time; render() hands them out in the
  // sizes the module asks for, so note-ons land on Dexed's 64-frame grid.
  float mix_[kBlockSize]{};
  float scratch_[kBlockSize]{};
  std::size_t pos_ = kBlockSize;
  int voices_ = 0;
};

}  // namespace ion
