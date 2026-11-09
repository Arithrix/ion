#include "engine/acid/acid_engine.h"

#include <algorithm>
#include <cmath>

#include "engine/envelope_params.h"
#include "rosic_Open303.h"

namespace ion {

namespace {

constexpr float kFine = 1.0f / 60.0f, kCoarse = 1.0f / 12.0f;
constexpr EnvDefaults kAmpDefaults{0.1f, kEnvFull, 1000, 1.0f, kEnvOff};
constexpr float kSilence = 3e-5f;   // about −90 dB
constexpr int kSleepFrames = 9600;  // 200 ms of that with no key down and it stops rendering
// Open303's accents add a lot of level (an accent with no resonance peaks
// ~9 dB above a plain note). −16 dB puts a typical squelchy line around −15
// dBFS with accents near −7, level with ion's other engines; the extreme
// (accents, resonance at 0) can still reach the master limiter.
constexpr double kHeadroomDb = -16.0;

const ParamInfo kParams[AcidEngine::kParamCount] = {
    {"waveform", "Waveform", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f, nullptr, "square", "saw"},
    {"cutoff", "Cutoff", 200, 5000, 800, Curve::Log, Unit::Hz, 1.0f / 120, 1.0f / 20},
    {"resonance", "Resonance", 0, 1, 0.7f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"envmod", "Env mod", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"decay", "Decay", 200, 2000, 400, Curve::Log, Unit::Ms, kFine, kCoarse},
    {"accent", "Accent", 0, 1, 0.6f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"slide", "Slide", 10, 500, 60, Curve::Log, Unit::Ms, kFine, kCoarse},
    {"accent.at", "Accent at", 1, 127, 100, Curve::Linear, Unit::None, 1, 10},
    {"tune", "Tune", -24, 24, 0, Curve::Linear, Unit::Semitones, 1, 12},
    {"fine", "Fine", -50, 50, 0, Curve::Linear, Unit::Cents, 1, 10},
    {"bend", "Bend range", 0, 24, 2, Curve::Choice, Unit::Semitones, 1, 12},
    {"level", "Level", -24, 12, 0, Curve::Linear, Unit::Db, 0.5f, 3},
    envelopeParam(false, kEnvAttack, kAmpDefaults),
    envelopeParam(false, kEnvHold, kAmpDefaults),
    envelopeParam(false, kEnvDecay, kAmpDefaults),
    envelopeParam(false, kEnvSustain, kAmpDefaults),
    envelopeParam(false, kEnvRelease, kAmpDefaults),
    velAmpParam(0),
};

}  // namespace

std::span<const ParamInfo> AcidEngine::paramTable() { return kParams; }

AcidEngine::AcidEngine() : v_(std::make_unique<rosic::Open303>()) {
  v_->setSampleRate(kSampleRate);
  for (int i = 0; i < kParamCount; ++i) setParam(i, kParams[i].def);
}

AcidEngine::~AcidEngine() = default;

void AcidEngine::updateTuning() {
  const double semis = values_[kTune] + values_[kFine] / 100.0;
  v_->setTuning(440.0 * std::pow(2.0, semis / 12.0));
}

void AcidEngine::setParam(int index, float value) {
  if (index < 0 || index >= kParamCount) return;
  values_[static_cast<std::size_t>(index)] = value;
  switch (index) {
    case kWaveform: v_->setWaveform(value); break;
    case kCutoff: v_->setCutoff(value); break;
    case kResonance: v_->setResonance(value * 100.0); break;
    case kEnvMod: v_->setEnvMod(value * 100.0); break;
    case kDecay: v_->setDecay(value); break;
    case kAccent: v_->setAccent(value * 100.0); break;
    case kSlide: v_->setSlideTime(value); break;
    case kAccentAt: v_->setAccentThreshold(static_cast<int>(std::lround(value))); break;
    case kTune:
    case kFine: updateTuning(); break;
    case kBendRange: v_->setPitchBend(bend_ * value); break;
    case kLevel: v_->setVolume(value + kHeadroomDb); break;
    default:
      if (index >= kFirstAmp && index < kVelAmp) ampCoeffs_ = envelopeCoeffs(&values_[kAmpAttack]);
      break;
  }
}

void AcidEngine::noteOn(uint8_t note, uint8_t velocity) {
  if (velocity == 0) {
    noteOff(note);
    return;
  }
  note &= 127;
  // A note while another is held slides to it (the 303's way); Open303 does
  // that itself. The amp envelope runs across the whole phrase.
  if (heldCount_ == 0) amp_.start();
  auto& word = held_[note >> 5];
  if (!(word & (1u << (note & 31)))) {
    word |= 1u << (note & 31);
    ++heldCount_;
  }
  velGain_ = velocityGain(values_[kVelAmp], static_cast<float>(velocity) / 127.0f);
  v_->noteOn(note, velocity, 0.0);
  awake_ = true;
  quiet_ = 0;
}

void AcidEngine::noteOff(uint8_t note) {
  note &= 127;
  auto& word = held_[note >> 5];
  if (!(word & (1u << (note & 31)))) return;
  word &= ~(1u << (note & 31));
  v_->noteOn(note, 0, 0.0);
  if (--heldCount_ == 0) amp_.release();
}

void AcidEngine::pitchBend(int value) {
  bend_ = static_cast<float>(value) / 8192.0f;
  v_->setPitchBend(bend_ * values_[kBendRange]);
}

void AcidEngine::allNotesOff() {
  v_->allNotesOff();
  amp_.reset();
  held_ = {};
  heldCount_ = 0;
  awake_ = false;
}

void AcidEngine::render(float* left, float* right, std::size_t frames) {
  if (!awake_) return;
  for (std::size_t i = 0; i < frames; ++i) {
    const float s = static_cast<float>(v_->getSample()) * amp_.next(ampCoeffs_) * velGain_;
    left[i] += s;
    right[i] += s;
    if (heldCount_ == 0 && std::fabs(s) < kSilence) {
      if (++quiet_ >= kSleepFrames) {
        awake_ = false;
        return;
      }
    } else {
      quiet_ = 0;
    }
  }
}

}  // namespace ion
