#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "dsp/ahdsr.h"
#include "engine/engine.h"
#include "engine/plaits/plaits_voice.h"

namespace ion {

// The 32 patches the "6-op FM a" model plays: packed DX7 voices, as Plaits
// reads them (HANDOFF §6.4: "6-op FM models read patches from the FM
// library"). Made on the control thread, owned by the engine once sent.
struct PlaitsBank {
  std::array<uint8_t, 4096> data{};
};

// Plaits, polyphonic (HANDOFF §6.4, DECISIONS D-56): N independent Plaits
// voices, each running the track's model, with the uniform amp envelope
// after its low-pass gate (D-50).
class PlaitsEngine final : public Engine {
 public:
  enum Param : int {
    kModel,
    kHarmonics, kTimbre, kMorph,
    kEnvPitch, kEnvTimbre, kEnvMorph,  // the internal decay envelope → pitch, timbre, morph (Plaits' attenuverters)
    kLpgMode, kLpgDecay, kLpgColour,
    kAux,     // out ↔ aux: each model's second output
    kVelLpg,  // velocity → level and LPG
    kPoly, kGlide, kBendRange,
    kAmpAttack, kAmpHold, kAmpDecay, kAmpSustain, kAmpRelease, kVelAmp,
    kParamCount
  };
  static constexpr int kFirstAmp = kAmpAttack;
  static constexpr int kMaxVoices = 8;
  static constexpr int kModels = 24;
  static std::span<const ParamInfo> paramTable();

  // ion's model order (the grid's: green, red, orange, eight each) → Plaits' own index.
  static int plaitsIndex(int model);
  static const char* modelName(int model);  // "String machine"
  // What harmonics, timbre and morph do on a model (0, 1, 2), in a few words.
  static const char* macroRole(int model, int macro);
  static const char* modelAbout(int model);  // one line about the model
  static bool isSixOp(int model) { return model >= 18 && model <= 20; }
  static bool isUserBank(int model) { return model == 18; }  // "6-op FM a" plays the track's own list
  static bool isChord(int model) { return model == 6 || model == 22 || model == 23; }
  // What Harmonics picks on the 6-op models (0-31) and the chord models (a name).
  static int sixOpSlot(float harmonics);
  static std::string chordName(float harmonics);  // "m7"
  // A patch in Plaits' own bank 0-2.
  static std::string builtInPatchName(int bank, int slot);

  // Allocates every voice; construct off the audio thread.
  PlaitsEngine();
  ~PlaitsEngine() override;

  EngineType type() const override { return EngineType::Plaits; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t, uint8_t) override {}
  void pitchBend(int value) override { bend_ = static_cast<float>(value) / 8192.0f; }
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override;

  // Real-time safe. Returns the bank it replaces (or null), which the caller
  // frees off the audio thread. Null goes back to Plaits' built-in bank.
  PlaitsBank* setBank(PlaitsBank* bank);

 private:
  struct Voice {
    std::unique_ptr<plaits_port::PlaitsVoice> plaits;
    dsp::Ahdsr amp;
    uint8_t note = 0;
    bool gate = false;     // key held
    bool on = false;       // sounding (allocated)
    float level = 0;       // from velocity, for the LPG
    float velGain = 1;     // velocity → amp
    float pitch = 0;       // the note (and glide) in semitones
    float glideFrom = 0;
    int glideLeft = 0;     // frames
    int quiet = 0;         // frames in a row near silence since release
    int cut = -1;          // frames left in a quick fade that ends the note (−1: none)
    uint32_t age = 0;
    std::array<float, plaits_port::PlaitsVoice::kBlock> out{}, aux{};
    std::size_t pos = plaits_port::PlaitsVoice::kBlock;  // next sample in out/aux
    bool active() const { return on; }
    bool released() const { return !gate; }
  };

  void start(Voice& v, uint8_t note, uint8_t velocity);
  void updateSettings();

  std::array<Voice, kMaxVoices> voices_;
  std::array<float, kParamCount> values_{};
  plaits_port::PlaitsVoice::Settings settings_;
  dsp::Ahdsr::Coeffs ampCoeffs_;
  std::unique_ptr<PlaitsBank> bank_;
  uint32_t age_ = 0;
  float bend_ = 0;
  float lastPitch_ = -1;  // the last note played, where the next glides from
};

}  // namespace ion
