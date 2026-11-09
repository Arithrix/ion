#include "engine/sampler/sampler_engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "engine/envelope_params.h"
#include "engine/voice_alloc.h"

namespace ion {
namespace {

constexpr float kSr = static_cast<float>(kSampleRate);
constexpr float kTimeFine = 1.0f / 60.0f;
constexpr float kTimeCoarse = 1.0f / 12.0f;
constexpr float kLogFine = 1.0f / 120.0f;
constexpr float kLogCoarse = 1.0f / 20.0f;

const char* const kModes[] = {"repitch"};  // "stretch" joins later (HANDOFF §6.2)
const char* const kLoops[] = {"off", "forward", "ping-pong"};
const char* const kFilters[] = {"off", "LP12", "LP24", "HP", "BP"};
const char* const kShapes[] = {"sine", "triangle", "square", "S&H"};
const char* const kDivisions[] = {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4D", "1/8D"};

constexpr EnvDefaults kAmp{2, 1, 500, 1, 200};    // hold 1 = none
constexpr EnvDefaults kFilt{2, 1, 400, 0, 300};

// Order must match SamplerEngine::Param.
const std::array<ParamInfo, SamplerEngine::kParamCount> kParams = {{
    {"mode", "Mode", 0, 0, 0, Curve::Choice, Unit::None, 1, 1, kModes},
    {"root", "Root", 0, 127, 60, Curve::Choice, Unit::Note, 1, 12},
    {"lowkey", "Low key", 0, 127, 0, Curve::Choice, Unit::Note, 1, 12},
    {"highkey", "High key", 0, 127, 127, Curve::Choice, Unit::Note, 1, 12},
    {"tune", "Tune", -24, 24, 0, Curve::Choice, Unit::Semitones, 1, 12},
    {"fine", "Fine", -50, 50, 0, Curve::Choice, Unit::Cents, 1, 10},
    {"level", "Level", -24, 24, 0, Curve::Linear, Unit::Db, 0.5f, 3},
    {"start", "Start", 0, 1, 0, Curve::Linear, Unit::Percent, 0.001f, 0.01f},
    {"loop", "Loop", 0, 2, 0, Curve::Choice, Unit::None, 1, 1, kLoops},
    {"loopstart", "Loop start", 0, 1, 0, Curve::Linear, Unit::Percent, 0.001f, 0.01f},
    {"loopend", "Loop end", 0, 1, 1, Curve::Linear, Unit::Percent, 0.001f, 0.01f},
    {"crossfade", "Crossfade", 0, 500, 0, Curve::Linear, Unit::Ms, 1, 10, nullptr, nullptr, "off"},
    {"snap", "Snap", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"glide", "Glide", 0, 2000, 0, Curve::Linear, Unit::Ms, 5, 50, nullptr, nullptr, "off"},
    {"poly", "Voices", 1, SamplerEngine::kMaxPolyphony, 8, Curve::Choice, Unit::None, 1, 4},
    {"bend", "Bend range", 0, 12, 2, Curve::Choice, Unit::None, 1, 2},
    envelopeParam(false, kEnvAttack, kAmp), envelopeParam(false, kEnvHold, kAmp), envelopeParam(false, kEnvDecay, kAmp),
    envelopeParam(false, kEnvSustain, kAmp), envelopeParam(false, kEnvRelease, kAmp), velAmpParam(0.6f),
    {"filter", "Filter", 0, 4, 0, Curve::Choice, Unit::None, 1, 1, kFilters},
    {"cutoff", "Cutoff", 20, 20000, 8000, Curve::Log, Unit::Hz, kLogFine, kLogCoarse},
    {"reso", "Resonance", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"keytrack", "Key track", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"env", "Env amount", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    envelopeParam(true, kEnvAttack, kFilt), envelopeParam(true, kEnvHold, kFilt), envelopeParam(true, kEnvDecay, kFilt),
    envelopeParam(true, kEnvSustain, kFilt), envelopeParam(true, kEnvRelease, kFilt),
    {"vel.filter", "Vel→filter", 0, 1, 0, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"lfo.shape", "LFO shape", 0, 3, 0, Curve::Choice, Unit::None, 1, 1, kShapes},
    {"lfo.sync", "LFO sync", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"lfo.rate", "LFO rate", 0.05f, 20, 2, Curve::Log, Unit::Hz, kTimeFine, kTimeCoarse},
    {"lfo.division", "LFO length", 0, 12, 4, Curve::Choice, Unit::None, 1, 3, kDivisions},
    {"lfo.pitch", "→ pitch", 0, 1200, 0, Curve::Linear, Unit::Cents, 5, 50},
    {"lfo.cutoff", "→ cutoff", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"lfo.pan", "→ pan", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"lfo.level", "→ level", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
}};

inline float hermite(const float* x, int64_t i, float f) {
  const float xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b = w + a;
  return ((a * f - b) * f + c) * f + x0;
}

inline float read(const Sample& s, int ch, double pos) {
  const auto i = static_cast<int64_t>(std::floor(pos));
  const int64_t clamped = std::clamp<int64_t>(i, 0, std::max<int64_t>(0, s.frames - 1));
  return hermite(s.channel(ch), clamped, static_cast<float>(pos - static_cast<double>(i)));
}

// −60 dB in `ms`: the per-sample coefficient of an exponential approach.
inline float coeff(float ms) { return std::exp(std::log(1e-3f) / (std::max(ms, 0.1f) * kSr / 1000.0f)); }

}  // namespace

std::span<const ParamInfo> SamplerEngine::paramTable() { return kParams; }

double SamplerEngine::divisionBeats(int d) {
  static const double beats[] = {16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75};
  return beats[std::clamp(d, 0, 12)];
}

SamplerEngine::SamplerEngine() {
  for (int i = 0; i < kParamCount; ++i) v_[static_cast<std::size_t>(i)] = kParams[static_cast<std::size_t>(i)].def;
}

void SamplerEngine::setParam(int index, float value) {
  if (index >= 0 && index < kParamCount) v_[static_cast<std::size_t>(index)] = value;
}

void SamplerEngine::setSample(int, const Sample* s) {
  const Sample* old = std::exchange(sample_, s);
  retirement_.retire(
      old,
      [&](const Sample* x) {
        for (Voice& v : voice_)
          if (v.sample == x) v.on = false;
      },
      [&](const Sample* x) { return inUse(x); });
}

bool SamplerEngine::inUse(const Sample* s) const {
  for (const Voice& v : voice_)
    if (v.on && v.sample == s) return true;
  return false;
}

void SamplerEngine::heldSamples(std::vector<const Sample*>& out) const {
  if (sample_) out.push_back(sample_);
  retirement_.forEach([&](const Sample* s) { out.push_back(s); });
}

void SamplerEngine::noteOn(uint8_t note, uint8_t velocity) {
  if (!sample_ || sample_->frames < 4) return;
  if (note < std::lround(v_[kLowKey]) || note > std::lround(v_[kHighKey])) return;

  // Voices: the polyphony setting counts notes still sounding; the oldest goes
  // (released ones first), faded rather than cut.
  const int poly = std::clamp(static_cast<int>(std::lround(v_[kPoly])), 1, kMaxPolyphony);
  for (;;) {
    int count = 0, victim = -1;
    for (int i = 0; i < kVoices; ++i) {
      const Voice& v = voice_[static_cast<std::size_t>(i)];
      if (!v.on || v.fading) continue;
      ++count;
      const Voice* best = victim < 0 ? nullptr : &voice_[static_cast<std::size_t>(victim)];
      if (!best || (v.released() && !best->released()) ||
          (v.released() == best->released() && static_cast<int32_t>(v.age - best->age) < 0))
        victim = i;
    }
    if (count < poly) break;
    voice_[static_cast<std::size_t>(victim)].fading = true;
  }

  Voice& v = voice_[static_cast<std::size_t>(pickVoice<Voice>(voice_))];
  v = Voice{};
  v.on = true;
  v.sample = sample_;
  v.note = note;
  v.velocity = static_cast<float>(velocity) / 127.0f;
  v.age = ++age_;
  v.pos = std::clamp(static_cast<double>(v_[kStart]), 0.0, 1.0) * static_cast<double>(sample_->frames - 1);
  v.pitch = v_[kGlide] > 0 && lastPitch_ >= 0 ? lastPitch_ : note;
  lastPitch_ = note;
  v.amp.start();
  v.filt.start();
}

void SamplerEngine::releaseVoice(Voice& v) {
  v.held = false;
  v.amp.release();
  v.filt.release();
}

void SamplerEngine::noteOff(uint8_t note) {
  for (Voice& v : voice_)
    if (v.on && v.keyDown && v.note == note) {
      v.keyDown = false;
      if (!sustain_) releaseVoice(v);
    }
}

void SamplerEngine::controlChange(uint8_t controller, uint8_t value) {
  switch (controller) {
    case 64:
      sustain_ = value >= 64;
      if (!sustain_)
        for (Voice& v : voice_)
          if (v.on && !v.keyDown && v.held) releaseVoice(v);
      break;
    case 120: allNotesOff(); break;
    case 123:
      for (Voice& v : voice_)
        if (v.on) {
          v.keyDown = false;
          releaseVoice(v);
        }
      break;
    default: break;
  }
}

void SamplerEngine::allNotesOff() {
  for (Voice& v : voice_) v.on = false;
  voices_ = 0;
  sustain_ = false;
  retirement_.update([&](const Sample* x) { return inUse(x); });
}

void SamplerEngine::render(float* left, float* right, std::size_t frames) {
  // The LFO: once per call, at control rate.
  const int shape = static_cast<int>(std::lround(v_[kLfoShape]));
  const double hz = v_[kLfoSync] >= 0.5f
                        ? std::clamp(bpm_, 20.0, 400.0) / 60.0 / divisionBeats(static_cast<int>(std::lround(v_[kLfoDivision])))
                        : static_cast<double>(v_[kLfoRate]);
  lfoPhase_ += hz * static_cast<double>(frames) / kSampleRate;
  if (lfoPhase_ >= 1) {
    lfoPhase_ -= std::floor(lfoPhase_);
    rng_ = rng_ * 1664525u + 1013904223u;
    lfoHold_ = static_cast<float>(rng_ >> 8) / 8388608.0f - 1.0f;
  }
  const auto ph = static_cast<float>(lfoPhase_);
  switch (shape) {
    case kSine: lfoValue_ = std::sin(2 * std::numbers::pi_v<float> * ph); break;
    case kTriangle: lfoValue_ = ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph; break;
    case kSquare: lfoValue_ = ph < 0.5f ? 1.0f : -1.0f; break;
    default: lfoValue_ = lfoHold_; break;
  }

  int n = 0;
  for (Voice& v : voice_) {
    if (!v.on) continue;
    renderVoice(v, left, right, frames, lfoValue_);
    n += v.on;
  }
  voices_ = n;
  if (retirement_.pending()) retirement_.update([&](const Sample* x) { return inUse(x); });
}

void SamplerEngine::renderVoice(Voice& v, float* left, float* right, std::size_t frames, float lfo) {
  const Sample& s = *v.sample;
  const double len = static_cast<double>(s.frames);

  // Glide toward the note, then the playback rate.
  const float glide = v_[kGlide];
  if (glide <= 0) v.pitch = v.note;
  else v.pitch = v.note + (v.pitch - v.note) * std::pow(coeff(glide), static_cast<float>(frames));
  const float semis = v.pitch - v_[kRoot] + v_[kTune] + v_[kFine] / 100.0f + bend_ * v_[kBendRange] + v_[kLfoPitch] / 100.0f * lfo;
  const double rate = std::exp2(static_cast<double>(semis) / 12.0);

  // Loop points in frames.
  const int loop = static_cast<int>(std::lround(v_[kLoop]));
  const double loopStart = std::clamp(static_cast<double>(v_[kLoopStart]), 0.0, 1.0) * (len - 1);
  const double loopEnd = std::clamp(static_cast<double>(v_[kLoopEnd]), 0.0, 1.0) * (len - 1);
  const bool looping = loop != kLoopOff && loopEnd - loopStart >= 16;
  const double loopLen = loopEnd - loopStart;
  const double xfade = loop == kLoopForward ? std::min({static_cast<double>(v_[kCrossfade]) * kSampleRate / 1000.0, loopLen / 2, loopStart}) : 0.0;

  // Amp: the envelopes, velocity, level, tremolo, pan.
  const dsp::Ahdsr::Coeffs ampC = envelopeCoeffs(&v_[kAmpAttack]), filtC = envelopeCoeffs(&v_[kFAttack]);
  float gain = std::pow(10.0f, v_[kLevel] / 20.0f) * velocityGain(v_[kVelAmp], v.velocity);
  gain *= 1 - v_[kLfoLevel] * (0.5f + 0.5f * lfo);
  const float pan = std::clamp(v_[kLfoPan] * lfo, -1.0f, 1.0f);
  const float theta = (pan + 1.0f) * std::numbers::pi_v<float> / 4.0f;
  const float gl = gain * std::numbers::sqrt2_v<float> * std::cos(theta), gr = gain * std::numbers::sqrt2_v<float> * std::sin(theta);

  // Filter, at the start of the block: envelope, key tracking, velocity, LFO, in octaves.
  const int filter = static_cast<int>(std::lround(v_[kFilter]));
  float a1 = 0, a2 = 0, a3 = 0, k = 2;
  float b1 = 0, b2 = 0, b3 = 0;  // LP24's second stage
  if (filter != kFilterOff) {
    const float octaves = v_[kEnvAmount] * 5.0f * v.filt.level() + v_[kKeyTrack] * (v.pitch - v_[kRoot]) / 12.0f +
                          v_[kLfoCutoff] * 4.0f * lfo - v_[kVelFilter] * 4.0f * (1.0f - v.velocity);
    const float fc = std::clamp(v_[kCutoff] * std::exp2(octaves), 10.0f, 0.45f * kSr);
    const float g = std::tan(std::numbers::pi_v<float> * fc / kSr);
    k = 2.0f - 1.94f * std::clamp(v_[kReso], 0.0f, 1.0f);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    // LP24's second stage resonates no further than flat (Butterworth, k = √2):
    // only the first one carries the resonance. Both resonating squared the
    // peak (Q 16 → ~270, +49 dB) and could drive the track far past full
    // scale (found by the 0.10.1 sweep). Below about 30% resonance nothing changes.
    const float k2 = std::max(k, std::numbers::sqrt2_v<float>);
    b1 = 1.0f / (1.0f + g * (g + k2));
    b2 = g * b1;
    b3 = g * b2;
  }
  const int stages = filter == kLp24 ? 2 : 1;

  const bool stereo = s.channels == 2;
  for (std::size_t i = 0; i < frames; ++i) {
    float x[2];
    for (int c = 0; c < (stereo ? 2 : 1); ++c) {
      x[c] = read(s, c, v.pos);
      if (xfade > 0 && v.pos > loopEnd - xfade) {
        // Crossfade the end of the loop into what precedes its start.
        const auto w = static_cast<float>((v.pos - (loopEnd - xfade)) / xfade);
        x[c] = x[c] * (1 - w) + read(s, c, v.pos - loopLen) * w;
      }
    }
    if (!stereo) x[1] = x[0];

    v.pos += rate * v.direction;
    if (looping && loop == kLoopForward) {
      while (v.pos >= loopEnd) v.pos -= loopLen;
    } else if (looping && loop == kLoopPingPong) {
      if (v.direction > 0 && v.pos >= loopEnd) {
        v.pos = loopEnd - (v.pos - loopEnd);
        v.direction = -1;
      } else if (v.direction < 0 && v.pos <= loopStart) {
        v.pos = loopStart + (loopStart - v.pos);
        v.direction = 1;
      }
    } else if (v.pos >= len - 1) {
      v.on = false;  // the end of the sample
      break;
    }

    const float env = v.amp.next(ampC);
    v.filt.next(filtC);
    if (filter != kFilterOff) {
      for (int c = 0; c < 2; ++c)
        for (int st = 0; st < stages; ++st) {
          float& s1 = v.s1[st][c];
          float& s2 = v.s2[st][c];
          const float in = x[c];
          const float c1 = st == 0 ? a1 : b1, c2 = st == 0 ? a2 : b2, c3 = st == 0 ? a3 : b3;
          const float v3 = in - s2;
          const float v1 = c1 * s1 + c2 * v3;
          const float v2 = s2 + c2 * s1 + c3 * v3;
          s1 = 2 * v1 - s1;
          s2 = 2 * v2 - s2;
          x[c] = filter == kHp ? in - k * v1 - v2 : filter == kBp ? k * v1 : v2;
        }
    }
    float e = env;
    if (v.fading) {
      v.fade -= 1.0f / kFadeFrames;
      if (v.fade <= 0) {
        v.on = false;
        break;
      }
      e *= v.fade;
    }
    left[i] += x[0] * e * gl;
    right[i] += x[1] * e * gr;
    if (v.amp.idle()) {
      v.on = false;
      break;
    }
  }
  if (!std::isfinite(v.s1[0][0] + v.s2[0][0] + v.s1[1][1] + v.s2[1][1])) {
    for (auto& st : v.s1) st[0] = st[1] = 0;
    for (auto& st : v.s2) st[0] = st[1] = 0;
  }
}

}  // namespace ion
