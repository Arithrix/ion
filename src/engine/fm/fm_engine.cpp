#include "engine/fm/fm_engine.h"

#include <array>
#include <cmath>

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
// for chords; the track fader does the rest.
constexpr float kOutputGain = 0.5f;

constexpr uint8_t kCcModWheel = 1;
constexpr uint8_t kCcBreath = 2;
constexpr uint8_t kCcFoot = 4;
constexpr uint8_t kCcSustain = 64;
constexpr uint8_t kCcAllSoundOff = 120;
constexpr uint8_t kCcAllNotesOff = 123;

const std::array<ParamInfo, FmEngine::kParamCount> kParams = {{
    {"poly", "Polyphony", 1, FmEngine::kMaxPolyphony, 8, Curve::Choice, Unit::None, 1, 4},
    {"bend", "Bend range", 0, 12, 2, Curve::Choice, Unit::Semitones, 1, 2},
    {"mono", "Mono", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
}};

}  // namespace

std::span<const ParamInfo> FmEngine::paramTable() { return kParams; }

FmEngine::FmEngine()
    : dexed_(std::make_unique<DexedCore>(static_cast<uint8_t>(kMaxPolyphony), static_cast<uint32_t>(kSampleRate))) {
  dexed_->setEngineType(MKI);  // Dexed's "Mark I" mode: closest to the original hardware
  for (int i = 0; i < kParamCount; ++i) setParam(i, kParams[static_cast<std::size_t>(i)].def);
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

void FmEngine::setParam(int index, float value) {
  const int v = static_cast<int>(std::lround(value));
  switch (index) {
    case kPoly: dexed_->setMaxNotes(static_cast<uint8_t>(v)); break;  // stops held notes
    case kBendRange: dexed_->setPitchbendRange(static_cast<uint8_t>(v)); break;
    case kMono: dexed_->setMonoMode(v != 0); break;
    default: break;
  }
}

void FmEngine::render(float* left, float* right, std::size_t frames) {
  std::size_t done = 0;
  while (done < frames) {
    if (pos_ == kBlockSize) {
      dexed_->getSamples(mono_, static_cast<uint16_t>(kBlockSize));
      // Also retires voices whose carriers have gone silent; without this call
      // Synth_Dexed keeps computing finished notes.
      voices_ = dexed_->getNumNotesPlaying();
      pos_ = 0;
    }
    const std::size_t n = std::min(frames - done, kBlockSize - pos_);
    for (std::size_t i = 0; i < n; ++i) {
      const float s = mono_[pos_ + i] * kOutputGain;
      left[done + i] += s;
      right[done + i] += s;
    }
    pos_ += n;
    done += n;
  }
}

void FmEngine::setClock(uint64_t frames) {
  g_ion_dexed_millis = static_cast<uint32_t>(frames * 1000 / kSampleRate);
}

void FmEngine::loadVoice(const syx::Voice& voice) {
  syx::Voice copy = voice;  // Dexed takes a non-const pointer
  dexed_->loadVoice(copy.data());
}

}  // namespace ion
