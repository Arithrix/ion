#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "dsp/ahdsr.h"
#include "engine/engine.h"
#include "engine/sample.h"
#include "engine/sample_retirement.h"

namespace ion {

// The drum sampler (HANDOFF §6.1): 16 pads, each a sample with its own
// pitch, level, pan, envelope, filter and drive, triggered by a MIDI note.
//
// Every pad parameter is an engine parameter, so each can be CC-mapped and is
// saved like any other: index = pad × kPadParamCount + PadParam.
//
// Each pad has the uniform AHDSR envelope (D-50). In one-shot mode note-off
// is ignored, so it runs attack, hold, decay, then sustain until the sample
// ends; in gate mode note-off starts the release.
//
// Pads are polyphonic up to kPadVoices; a pad in a choke group stops every
// voice in that group when it plays, itself included, so a pad alone in a
// group is monophonic. Stopping a voice (choke, a fifth hit, a steal) fades it
// over kFadeFrames rather than cutting it.
class DrumEngine final : public Engine {
 public:
  static constexpr int kPads = 16;
  static constexpr int kFirstNote = 36;  // C1: pads 1-16 on C1-D#2
  enum PadParam : int {
    kNote, kTune, kFine, kLevel, kPan, kStart, kReverse, kMode,
    kAttack, kHold, kDecay, kSustain, kRelease,  // the uniform envelope (D-50)
    kFilter, kCutoff, kReso, kDrive, kVelAmp, kVelFilter, kChoke,
    kPadParamCount
  };
  enum Mode : int { kOneShot, kGate };
  enum Filter : int { kFilterOff, kLowPass, kHighPass, kBandPass };
  static constexpr int kParamCount = kPads * kPadParamCount;
  static constexpr int param(int pad, int p) { return pad * kPadParamCount + p; }

  // All kParamCount parameters ("pad5.tune", "Pad 5 tune").
  static std::span<const ParamInfo> paramTable();
  // One pad's parameters with short labels ("tune", "Tune"), for the pad editor.
  static std::span<const ParamInfo> padParamTable();

  static constexpr int kVoices = 64;
  static constexpr int kPadVoices = 4;
  static constexpr int kFadeFrames = 240;     // 5 ms
  static constexpr int kDeclickFrames = 48;   // 1 ms fade-in when the start point is moved

  DrumEngine();

  EngineType type() const override { return EngineType::Drum; }
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t controller, uint8_t value) override;
  void pitchBend(int) override {}
  void allNotesOff() override;
  void setParam(int index, float value) override;
  void render(float* left, float* right, std::size_t frames) override;
  int voiceCount() const override { return voices_; }

  // ---- samples (audio thread) ----
  // Puts `s` (may be null) on a pad. The pad's previous sample goes on the
  // release list once no voice is playing it any more.
  void setSample(int pad, const Sample* s) override;
  // A sample this engine has finished with; the module hands each back to the
  // control thread (Notification::SampleReleased). False when there's none.
  bool popReleased(const Sample*& s) override { return retirement_.pop(s); }
  void heldSamples(std::vector<const Sample*>& out) const override {
    forEachSample([&](const Sample* s) { out.push_back(s); });
  }
  // Bit p set if pad p plays on `note`.
  uint16_t padsForNote(uint8_t note) const { return noteMap_[note]; }

  // Every sample reference the engine still holds: one per pad that has one,
  // plus those waiting to be released. Only for an engine the audio thread has
  // handed back (it's then no longer running).
  template <class F>
  void forEachSample(F&& f) const {
    for (const Pad& p : pads_)
      if (p.sample) f(p.sample);
    retirement_.forEach(f);
  }

  // For tests.
  const Sample* sample(int pad) const { return pads_[static_cast<std::size_t>(pad)].sample; }
  float value(int pad, int p) const { return pads_[static_cast<std::size_t>(pad)].v[static_cast<std::size_t>(p)]; }

 private:
  struct Pad {
    const Sample* sample = nullptr;
    std::array<float, kPadParamCount> v{};
  };
  struct Voice {
    bool on = false;
    bool fading = false;     // choked, stolen or over the pad's limit: 5 ms to silence
    float fade = 1;
    const Sample* sample = nullptr;
    int pad = 0;
    uint8_t note = 0;
    bool gate = false;       // gate mode, key still down
    uint32_t age = 0;
    double pos = 0;          // frame in the sample
    int direction = 1;       // −1 reversed
    float velocity = 1;      // 0…1
    dsp::Ahdsr amp;
    int declick = 0;         // frames of fade-in left
    float gainL = 0, gainR = 0;  // current, ramped to the pad's level and pan
    float cutoffScale = 1;   // velocity → filter
    float s1L = 0, s2L = 0, s1R = 0, s2R = 0;  // filter state
    bool active() const { return on; }
    bool released() const { return fading || amp.stage() == dsp::Ahdsr::Stage::Release || amp.stage() == dsp::Ahdsr::Stage::Decay; }
  };

  void start(int pad, uint8_t note, float velocity);
  void fade(Voice& v);
  void renderVoice(Voice& v, float* left, float* right, std::size_t frames);
  bool inUse(const Sample* s) const;
  void rebuildNoteMap();

  std::array<Pad, kPads> pads_{};
  std::array<Voice, kVoices> voice_{};
  std::array<uint16_t, 128> noteMap_{};
  uint32_t age_ = 0;
  int voices_ = 0;

  // Samples taken off pads, still maybe in use by voices.
  SampleRetirement<64> retirement_;
};

}  // namespace ion
