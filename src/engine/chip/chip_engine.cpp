#include "engine/chip/chip_engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/envelope_params.h"
#include "engine/voice_alloc.h"

namespace ion {

namespace {

constexpr float kFine = 1.0f / 60.0f, kCoarse = 1.0f / 12.0f;
constexpr float kVoiceGain = 0.35f;  // a full-scale square is loud: six of them still leave headroom
constexpr int kCutFrames = 240;      // 5 ms
// Short release by default: the oscillator never ends on its own.
constexpr EnvDefaults kAmpDefaults{0.1f, kEnvFull, 500, 1.0f, 40};

const char* const kShapes[] = {"pulse 12%", "pulse 25%", "pulse 50%", "pulse 75%", "saw", "triangle", "sine", "noise tone", "noise"};
const char* const kLimits[] = {"clip", "sine", "fold", "wrap"};
const char* const kFilters[] = {"off", "low-pass", "high-pass", "band-pass", "notch"};

const ParamInfo kParams[ChipEngine::kParamCount] = {
    {"shape", "Shape", 0, ChipEngine::kShapeCount - 1, ChipEngine::kPulse50, Curve::Choice, Unit::None, 1, 1, kShapes},
    {"size", "Size", 4, ChipEngine::kMaxSize, 32, Curve::Linear, Unit::None, 1, 16},
    {"mult", "Mult", 1, 16, 1, Curve::Linear, Unit::None, 1, 4},
    {"warp", "Warp", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"mirror", "Mirror", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f, nullptr, nullptr, "off"},
    {"amp", "Amp", 0, 24, 0, Curve::Linear, Unit::Db, 0.5f, 3},
    {"limit", "Limit", 0, 3, 0, Curve::Choice, Unit::None, 1, 1, kLimits},
    {"bits", "Bits", 1, 8, 8, Curve::Linear, Unit::None, 1, 1},
    {"filter", "Filter", 0, 4, 0, Curve::Choice, Unit::None, 1, 1, kFilters},
    {"cutoff", "Cutoff", 30, 18000, 6000, Curve::Log, Unit::Hz, 1.0f / 120, 1.0f / 20},
    {"resonance", "Resonance", 0, 1, 0.2f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"rate", "Rate", 1000, 48000, 48000, Curve::Log, Unit::Hz, 1.0f / 120, 1.0f / 20, nullptr, "full"},
    {"tune", "Tune", -24, 24, 0, Curve::Linear, Unit::Semitones, 1, 12},
    {"fine", "Fine", -50, 50, 0, Curve::Linear, Unit::Cents, 1, 10},
    {"glide", "Glide", 0.1f, 2000, 0.1f, Curve::Log, Unit::Ms, kFine, kCoarse, nullptr, nullptr, "off"},
    {"poly", "Voices", 1, ChipEngine::kMaxVoices, 6, Curve::Choice, Unit::None, 1, 1},
    {"bend", "Bend range", 0, 24, 2, Curve::Choice, Unit::Semitones, 1, 12},
    {"level", "Level", -24, 12, 0, Curve::Linear, Unit::Db, 0.5f, 3},
    envelopeParam(false, kEnvAttack, kAmpDefaults),
    envelopeParam(false, kEnvHold, kAmpDefaults),
    envelopeParam(false, kEnvDecay, kAmpDefaults),
    envelopeParam(false, kEnvSustain, kAmpDefaults),
    envelopeParam(false, kEnvRelease, kAmpDefaults),
    velAmpParam(0.5f),
};

double frac(double x) { return x - std::floor(x); }

}  // namespace

std::span<const ParamInfo> ChipEngine::paramTable() { return kParams; }

ChipEngine::ChipEngine() {
  for (int i = 0; i < kParamCount; ++i) values_[static_cast<std::size_t>(i)] = kParams[i].def;
  ampCoeffs_ = envelopeCoeffs(&values_[kAmpAttack]);
  svfCoeffs_ = dsp::SvfCoeffs::make(values_[kCutoff], values_[kResonance], kSampleRate);
  rebuildIfNeeded();
}

void ChipEngine::setParam(int index, float value) {
  if (index < 0 || index >= kParamCount) return;
  float& v = values_[static_cast<std::size_t>(index)];
  if (v == value) return;
  v = value;
  if (index <= kBits) dirty_ = true;  // shape … bits make the table
  else if (index == kCutoff || index == kResonance) svfCoeffs_ = dsp::SvfCoeffs::make(values_[kCutoff], values_[kResonance], kSampleRate);
  else if (index >= kAmpAttack && index < kVelAmp) ampCoeffs_ = envelopeCoeffs(&values_[kAmpAttack]);
  else if (index == kPoly) {
    const int n = std::clamp(static_cast<int>(std::lround(value)), 1, kMaxVoices);
    for (int i = n; i < kMaxVoices; ++i)
      if (voices_[static_cast<std::size_t>(i)].on && voices_[static_cast<std::size_t>(i)].cut < 0) {
        voices_[static_cast<std::size_t>(i)].gate = false;
        voices_[static_cast<std::size_t>(i)].cut = kCutFrames;
      }
  }
}

// One cycle of the basic shape at phase p (0…1), −1…1.
float ChipEngine::shapeAt(double p) const {
  switch (static_cast<int>(std::lround(values_[kShape]))) {
    case kPulse12: return p < 0.125 ? 1.0f : -1.0f;
    case kPulse25: return p < 0.25 ? 1.0f : -1.0f;
    case kPulse50: return p < 0.5 ? 1.0f : -1.0f;
    case kPulse75: return p < 0.75 ? 1.0f : -1.0f;
    case kSaw: return static_cast<float>(2 * p - 1);
    case kTriangle: return static_cast<float>(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4);
    case kSine: return static_cast<float>(std::sin(2 * std::numbers::pi * p));
    default: return 0;  // the noises don't use the table
  }
}

// Amp into the limiter, then down to the bit depth.
float ChipEngine::limitAndQuantize(float v) const {
  v *= std::pow(10.0f, values_[kAmp] / 20.0f);
  switch (static_cast<int>(std::lround(values_[kLimit]))) {
    case kSineLimit: v = std::sin(v * std::numbers::pi_v<float> * 0.5f); break;
    case kFold: {
      // Fold back at ±1, as many times as it takes.
      float x = std::fmod(v + 1.0f, 4.0f);
      if (x < 0) x += 4.0f;
      v = x < 2.0f ? x - 1.0f : 3.0f - x;
      break;
    }
    case kWrap: {
      float x = std::fmod(v + 1.0f, 2.0f);
      if (x < 0) x += 2.0f;
      v = x - 1.0f;
      break;
    }
    default: v = std::clamp(v, -1.0f, 1.0f); break;
  }
  const float levels = std::exp2(std::round(values_[kBits])) - 1.0f;
  return std::round((v + 1.0f) * 0.5f * levels) / levels * 2.0f - 1.0f;
}

void ChipEngine::rebuildIfNeeded() {
  if (!dirty_) return;
  dirty_ = false;
  size_ = std::clamp(static_cast<int>(std::lround(values_[kSize])), 4, kMaxSize);
  const double mult = std::round(values_[kMult]);
  const double warp = std::exp2(-3.0 * values_[kWarp]);
  const double mirror = values_[kMirror];
  for (int i = 0; i < size_; ++i) {
    double p = (i + 0.5) / size_;
    // Mirror: past the point, the wave runs back the way it came.
    if (mirror > 0) {
      const double m = 1.0 - 0.999 * mirror;
      if (p > m) p = m * (1.0 - (p - m) / (1.0 - m + 1e-9));
    }
    p = std::pow(p, warp);    // warp: pushes the shape to one side
    p = frac(p * mult);       // mult: repeats it
    table_[static_cast<std::size_t>(i)] = limitAndQuantize(shapeAt(p));
  }
  noiseHigh_ = limitAndQuantize(1.0f);
  noiseLow_ = limitAndQuantize(-1.0f);
}

int ChipEngine::voiceCount() const {
  int n = 0;
  for (const Voice& v : voices_) n += v.on;
  return n;
}

void ChipEngine::start(Voice& v, uint8_t note, uint8_t velocity) {
  v.note = note;
  v.gate = true;
  v.velGain = velocityGain(values_[kVelAmp], static_cast<float>(velocity) / 127.0f);
  if (values_[kGlide] > 0.1f && lastPitch_ >= 0) {
    v.glideFrom = lastPitch_;
    v.glideLeft = static_cast<int>(values_[kGlide] * kSampleRate / 1000.0f);
  } else {
    v.glideLeft = 0;
  }
  v.pitch = note;
  lastPitch_ = note;
  if (!v.on) {
    v.phase = 0;
    v.svf.reset();
  }
  v.on = true;
  v.cut = -1;
  v.age = ++age_;
  v.amp.start();
}

void ChipEngine::noteOn(uint8_t note, uint8_t velocity) {
  if (velocity == 0) {
    noteOff(note);
    return;
  }
  const int n = std::clamp(static_cast<int>(std::lround(values_[kPoly])), 1, kMaxVoices);
  for (int i = 0; i < n; ++i)
    if (voices_[static_cast<std::size_t>(i)].on && voices_[static_cast<std::size_t>(i)].note == note) {
      start(voices_[static_cast<std::size_t>(i)], note, velocity);
      return;
    }
  start(voices_[static_cast<std::size_t>(pickVoice(std::span<const Voice>(voices_.data(), static_cast<std::size_t>(n))))], note, velocity);
}

void ChipEngine::noteOff(uint8_t note) {
  for (Voice& v : voices_)
    if (v.on && v.gate && v.note == note) {
      v.gate = false;
      v.amp.release();
    }
}

void ChipEngine::allNotesOff() {
  for (Voice& v : voices_) {
    v.on = v.gate = false;
    v.amp.reset();
  }
}

void ChipEngine::render(float* left, float* right, std::size_t frames) {
  rebuildIfNeeded();
  const int shape = static_cast<int>(std::lround(values_[kShape]));
  const bool noise = shape == kNoiseTone || shape == kNoise;
  const int filter = static_cast<int>(std::lround(values_[kFilter]));
  const auto mode = filter == kHighPass ? dsp::Svf::Mode::HighPass
                    : filter == kBandPass ? dsp::Svf::Mode::BandPass
                    : filter == kNotch    ? dsp::Svf::Mode::Notch
                                          : dsp::Svf::Mode::LowPass;
  const float gain = kVoiceGain * std::pow(10.0f, values_[kLevel] / 20.0f);
  const float offset = values_[kTune] + values_[kFine] / 100.0f + bend_ * values_[kBendRange];
  const bool releaseOff = ampCoeffs_.release >= 1.0f;
  const float glideTotal = values_[kGlide] * kSampleRate / 1000.0f;
  // Noise: the noise-tone shift register loops every 93 steps, so it's clocked
  // 93 times a cycle to sound at the note; plain noise is clocked 8 times a
  // cycle, so higher notes are brighter noise.
  const double noiseSteps = shape == kNoiseTone ? 93.0 : 8.0;
  const float* table = table_.data();
  const double size = size_;

  float mix[kBlockSize] = {};  // the voices, mono; frames ≤ kBlockSize
  for (Voice& v : voices_) {
    if (!v.on) continue;
    // Pitch once per call (≤ 64 frames): glide moves in steps that small.
    float pitch = v.pitch;
    if (v.glideLeft > 0) {
      pitch = v.pitch + (v.glideFrom - v.pitch) * static_cast<float>(v.glideLeft) / std::max(1.0f, glideTotal);
      v.glideLeft = std::max(0, v.glideLeft - static_cast<int>(frames));
    }
    const double inc = 440.0 * std::exp2((pitch + offset - 69.0) / 12.0) / kSampleRate;
    for (std::size_t i = 0; i < frames; ++i) {
      float s;
      if (noise) {
        v.noisePhase += inc * noiseSteps;
        while (v.noisePhase >= 1.0) {
          v.noisePhase -= 1.0;
          // 15-bit LFSR (as the NES): tap bit 6 for the short, tonal loop, bit 1 for long noise.
          const int tap = shape == kNoiseTone ? 6 : 1;
          const uint16_t bit = static_cast<uint16_t>((v.lfsr ^ (v.lfsr >> tap)) & 1u);
          v.lfsr = static_cast<uint16_t>((v.lfsr >> 1) | (bit << 14));
          v.noiseOut = (v.lfsr & 1u) ? noiseHigh_ : noiseLow_;
        }
        s = v.noiseOut;
      } else {
        v.phase += inc;
        if (v.phase >= 1.0) v.phase -= std::floor(v.phase);
        s = table[static_cast<int>(v.phase * size)];
      }
      if (filter != kFilterOff) s = v.svf.process(s, svfCoeffs_, mode);
      float env = v.amp.next(ampCoeffs_);
      if (!v.gate && releaseOff && v.cut < 0) v.cut = kCutFrames;
      if (v.cut >= 0) {
        env *= static_cast<float>(v.cut) / kCutFrames;
        if (v.cut-- == 0) {
          v.on = false;
          break;
        }
      }
      mix[i] += s * env * v.velGain * gain;
      if (!v.gate && v.amp.idle()) {
        v.on = false;
        break;
      }
    }
  }
  // The track's sample rate: hold each sample for 48 kHz ÷ rate.
  const double step = values_[kRate] / kSampleRate;
  for (std::size_t i = 0; i < frames; ++i) {
    float s = mix[i];
    if (step < 1.0) {
      holdPhase_ += step;
      if (holdPhase_ >= 1.0) {
        holdPhase_ -= 1.0;
        holdL_ = s;
      }
      s = holdL_;
    }
    left[i] += s;
    right[i] += s;
  }
}

}  // namespace ion
