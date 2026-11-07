#include "engine/fm/fm_engine.h"

#include <dexed.h>

uint32_t g_ion_dexed_millis = 0;

namespace ion {

// Synth_Dexed renders through a protected method; it expects to be subclassed.
class DexedCore : public Dexed {
 public:
  using Dexed::Dexed;
  using Dexed::getSamples;

  // loadVoiceParameters() stops every voice, then flags a "refresh" for live
  // notes. The refresh (Env::update) jumps each envelope straight to stage 3,
  // so any note started before the next render would lose its attack and
  // decay. Nothing is live after the load, so the refresh is never needed.
  void loadVoice(uint8_t* voice) {
    loadVoiceParameters(voice);
    refreshVoice = false;
  }
};

namespace {

// Synth_Dexed outputs about ±0.5 per voice at full level. This leaves headroom
// for chords before the track fader (Phase 1) takes over.
constexpr float kOutputGain = 0.5f;

constexpr uint8_t kCcModWheel = 1;
constexpr uint8_t kCcBreath = 2;
constexpr uint8_t kCcFoot = 4;
constexpr uint8_t kCcSustain = 64;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcAllNotesOff = 123;

}  // namespace

FmEngine::FmEngine(int polyphony)
    : dexed_(std::make_unique<DexedCore>(static_cast<uint8_t>(polyphony), static_cast<uint32_t>(kSampleRate))) {
  dexed_->setEngineType(MKI);  // Dexed's "Mark I" mode: closest to the original hardware
  dexed_->setPitchbendRange(2);
  dexed_->activate();
}

FmEngine::~FmEngine() = default;

void FmEngine::noteOn(uint8_t note, uint8_t velocity) { dexed_->keydown(note, velocity); }

void FmEngine::noteOff(uint8_t note) { dexed_->keyup(note); }

void FmEngine::controlChange(uint8_t controller, uint8_t value) {
  switch (controller) {
    case kCcModWheel: dexed_->setModWheel(value); dexed_->ControllersRefresh(); break;
    case kCcBreath: dexed_->setBreathController(value); dexed_->ControllersRefresh(); break;
    case kCcFoot: dexed_->setFootController(value); dexed_->ControllersRefresh(); break;
    case kCcSustain: dexed_->setSustain(value >= 64); break;
    case kCcAllSoundOff: allNotesOff(); break;
    case kCcAllNotesOff: dexed_->notesOff(); break;
    default: break;
  }
}

void FmEngine::pitchBend(int value) { dexed_->setPitchbend(static_cast<int16_t>(value)); }

void FmEngine::allNotesOff() { dexed_->panic(); }

void FmEngine::render(float* left, float* right) {
  dexed_->getSamples(mono_, static_cast<uint16_t>(kBlockSize));
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const float s = mono_[i] * kOutputGain;
    left[i] += s;
    right[i] += s;
  }
  // Also retires voices whose carriers have gone silent; without this call
  // Synth_Dexed keeps computing finished notes.
  voices_ = dexed_->getNumNotesPlaying();
}

void FmEngine::setClock(uint64_t frames) {
  g_ion_dexed_millis = static_cast<uint32_t>(frames * 1000 / kSampleRate);
}

void FmEngine::loadVoice(const syx::Voice& voice) {
  syx::Voice copy = voice;  // Dexed takes a non-const pointer
  dexed_->loadVoice(copy.data());
}

}  // namespace ion
