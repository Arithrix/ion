#include "engine/drum/drum_engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include "engine/envelope_params.h"
#include "engine/voice_alloc.h"

namespace ion {
namespace {

constexpr float kLogFine = 1.0f / 120.0f;
constexpr float kLogCoarse = 1.0f / 20.0f;

const char* const kModes[] = {"one-shot", "gate"};
const char* const kFilters[] = {"off", "LP", "HP", "BP"};
const char* const kChokes[] = {"none", "A", "B", "C", "D"};

// A drum hit: no attack, held to the end of the sample, no sustain; release for gate mode.
constexpr EnvDefaults kEnv{0.1f, kEnvFull, 500, 0, 200};

// Order must match DrumEngine::PadParam.
const std::array<ParamInfo, DrumEngine::kPadParamCount> kPad = {{
    {"note", "Note", 0, 127, DrumEngine::kFirstNote, Curve::Choice, Unit::Note, 1, 12},
    {"tune", "Tune", -24, 24, 0, Curve::Choice, Unit::Semitones, 1, 12},
    {"fine", "Fine", -50, 50, 0, Curve::Choice, Unit::Cents, 1, 10},
    {"level", "Level", kFaderMinDb, 6.0f, 0.0f, Curve::Fader, Unit::Db, 0.005f, 0.05f},
    {"pan", "Pan", -1.0f, 1.0f, 0.0f, Curve::Linear, Unit::Pan, 0.05f, 0.25f},
    {"start", "Start", 0, 1000, 0, Curve::Linear, Unit::Ms, 1, 10},
    {"reverse", "Reverse", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"mode", "Mode", 0, 1, 0, Curve::Choice, Unit::None, 1, 1, kModes},
    envelopeParam(false, kEnvAttack, kEnv), envelopeParam(false, kEnvHold, kEnv), envelopeParam(false, kEnvDecay, kEnv),
    envelopeParam(false, kEnvSustain, kEnv), envelopeParam(false, kEnvRelease, kEnv),
    {"filter", "Filter", 0, 3, 0, Curve::Choice, Unit::None, 1, 1, kFilters},
    {"cutoff", "Cutoff", 20, 20000, 5000, Curve::Log, Unit::Hz, kLogFine, kLogCoarse},
    {"reso", "Resonance", 0, 1, 0, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"drive", "Drive", 0, 1, 0, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    velAmpParam(0.5f),
    {"vel.filter", "Vel→filter", 0, 1, 0, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"choke", "Choke", 0, 4, 0, Curve::Choice, Unit::None, 1, 1, kChokes},
}};

struct Table {
  std::array<std::string, DrumEngine::kParamCount> keys, labels;
  std::array<ParamInfo, DrumEngine::kParamCount> info{};
  Table() {
    for (int pad = 0; pad < DrumEngine::kPads; ++pad)
      for (int p = 0; p < DrumEngine::kPadParamCount; ++p) {
        const auto i = static_cast<std::size_t>(DrumEngine::param(pad, p));
        ParamInfo pi = kPad[static_cast<std::size_t>(p)];
        keys[i] = "pad" + std::to_string(pad + 1) + "." + pi.key;
        std::string label = pi.label;
        label[0] = static_cast<char>(std::tolower(label[0]));
        labels[i] = "Pad " + std::to_string(pad + 1) + " " + label;
        pi.key = keys[i].c_str();
        pi.label = labels[i].c_str();
        if (p == DrumEngine::kNote) pi.def = static_cast<float>(DrumEngine::kFirstNote + pad);
        info[i] = pi;
      }
  }
};

const Table& table() {
  static const Table t;
  return t;
}

// 4-point, 3rd-order Hermite.
inline float hermite(const float* x, int64_t i, float f) {
  const float xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b = w + a;
  return ((a * f - b) * f + c) * f + x0;
}

// tanh-like, exact at ±3 and beyond.
inline float softClip(float x) {
  x = std::clamp(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Constant-power pan, unity at centre (the same law as the mixer).
inline void panGains(float pan, float& l, float& r) {
  const float theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * std::numbers::pi_v<float> / 4.0f;
  l = std::numbers::sqrt2_v<float> * std::cos(theta);
  r = std::numbers::sqrt2_v<float> * std::sin(theta);
}

}  // namespace

std::span<const ParamInfo> DrumEngine::paramTable() { return table().info; }
std::span<const ParamInfo> DrumEngine::padParamTable() { return kPad; }

DrumEngine::DrumEngine() {
  for (int i = 0; i < kParamCount; ++i) setParam(i, table().info[static_cast<std::size_t>(i)].def);
}

void DrumEngine::setParam(int index, float value) {
  if (index < 0 || index >= kParamCount) return;
  const int pad = index / kPadParamCount, p = index % kPadParamCount;
  pads_[static_cast<std::size_t>(pad)].v[static_cast<std::size_t>(p)] = value;
  if (p == kNote) rebuildNoteMap();
}

void DrumEngine::rebuildNoteMap() {
  noteMap_.fill(0);
  for (int pad = 0; pad < kPads; ++pad) {
    const int n = std::clamp(static_cast<int>(std::lround(pads_[static_cast<std::size_t>(pad)].v[kNote])), 0, 127);
    noteMap_[static_cast<std::size_t>(n)] |= static_cast<uint16_t>(1u << pad);
  }
}

void DrumEngine::noteOn(uint8_t note, uint8_t velocity) {
  const uint16_t pads = noteMap_[note & 127];
  for (int pad = 0; pad < kPads; ++pad)
    if (pads & (1u << pad)) start(pad, note, static_cast<float>(velocity) / 127.0f);
}

void DrumEngine::start(int pad, uint8_t note, float velocity) {
  const Pad& p = pads_[static_cast<std::size_t>(pad)];
  if (!p.sample || p.sample->frames <= 0) return;

  // Choke: everything in this pad's group stops, this pad included.
  const int group = static_cast<int>(std::lround(p.v[kChoke]));
  if (group > 0)
    for (Voice& v : voice_)
      if (v.active() && !v.fading &&
          std::lround(pads_[static_cast<std::size_t>(v.pad)].v[kChoke]) == group)
        fade(v);
  // At most kPadVoices per pad: the oldest one goes.
  int count = 0, oldest = -1;
  for (int i = 0; i < kVoices; ++i) {
    const Voice& v = voice_[static_cast<std::size_t>(i)];
    if (!v.active() || v.pad != pad || v.fading) continue;
    ++count;
    if (oldest < 0 || static_cast<int32_t>(v.age - voice_[static_cast<std::size_t>(oldest)].age) < 0) oldest = i;
  }
  if (count >= kPadVoices) fade(voice_[static_cast<std::size_t>(oldest)]);

  const int i = pickVoice<Voice>(voice_);
  Voice& v = voice_[static_cast<std::size_t>(i)];
  v = Voice{};
  v.sample = p.sample;
  v.pad = pad;
  v.note = note;
  v.age = ++age_;
  v.velocity = velocity;
  v.direction = p.v[kReverse] >= 0.5f ? -1 : 1;
  const double start = std::max(0.0, static_cast<double>(p.v[kStart]) * kSampleRate / 1000.0);
  if (start >= static_cast<double>(p.sample->frames)) return;  // starts past the end: silent
  v.pos = v.direction > 0 ? start : static_cast<double>(p.sample->frames - 1) - start;
  v.declick = start > 0 ? kDeclickFrames : 0;
  v.gate = std::lround(p.v[kMode]) == kGate;
  v.on = true;
  v.amp.start();
  v.cutoffScale = std::exp2(-p.v[kVelFilter] * 4.0f * (1.0f - velocity));  // soft hits: up to 4 octaves darker
  // Start at the pad's gains, so the first block doesn't ramp up from zero.
  const float gain = faderGain(p.v[kLevel]) * velocityGain(p.v[kVelAmp], velocity);
  float pl, pr;
  panGains(p.v[kPan], pl, pr);
  v.gainL = gain * pl;
  v.gainR = gain * pr;
}

void DrumEngine::fade(Voice& v) {
  v.fading = true;
}

void DrumEngine::noteOff(uint8_t note) {
  for (Voice& v : voice_)
    if (v.active() && v.gate && v.note == note) {
      v.gate = false;
      v.amp.release();
    }
}

void DrumEngine::controlChange(uint8_t controller, uint8_t) {
  if (controller == 120) allNotesOff();  // all sound off
  if (controller == 123)                 // all notes off: let gated pads decay
    for (Voice& v : voice_)
      if (v.active() && v.gate) noteOff(v.note);
}

void DrumEngine::allNotesOff() {
  for (Voice& v : voice_) v.on = false;
  voices_ = 0;
  retirement_.update([&](const Sample* x) { return inUse(x); });
}

void DrumEngine::setSample(int pad, const Sample* s) {
  if (pad < 0 || pad >= kPads) return;
  const Sample* old = std::exchange(pads_[static_cast<std::size_t>(pad)].sample, s);
  retirement_.retire(
      old,
      [&](const Sample* x) {
        for (Voice& v : voice_)
          if (v.sample == x) v.on = false;
      },
      [&](const Sample* x) { return inUse(x); });
}

bool DrumEngine::inUse(const Sample* s) const {
  for (const Voice& v : voice_)
    if (v.active() && v.sample == s) return true;
  return false;
}

void DrumEngine::render(float* left, float* right, std::size_t frames) {
  int n = 0;
  for (Voice& v : voice_) {
    if (!v.active()) continue;
    renderVoice(v, left, right, frames);
    n += v.active();
  }
  voices_ = n;
  if (retirement_.pending()) retirement_.update([&](const Sample* x) { return inUse(x); });
}

void DrumEngine::renderVoice(Voice& v, float* left, float* right, std::size_t frames) {
  const Pad& p = pads_[static_cast<std::size_t>(v.pad)];
  const Sample& s = *v.sample;

  // Pad settings are read every block, so moving one (or a CC on it) changes
  // notes already sounding; gains ramp across the block to avoid zipper noise.
  const double rate = std::exp2((static_cast<double>(p.v[kTune]) + p.v[kFine] / 100.0) / 12.0) * v.direction;
  const float gain = faderGain(p.v[kLevel]) * velocityGain(p.v[kVelAmp], v.velocity);
  float pl, pr;
  panGains(p.v[kPan], pl, pr);
  const float inv = 1.0f / static_cast<float>(frames);
  const float dl = (gain * pl - v.gainL) * inv, dr = (gain * pr - v.gainR) * inv;
  const dsp::Ahdsr::Coeffs env = envelopeCoeffs(&p.v[kAttack]);

  const int filter = static_cast<int>(std::lround(p.v[kFilter]));
  float a1 = 0, a2 = 0, a3 = 0, k = 2;
  if (filter != kFilterOff) {
    // Topology-preserving SVF (Simper). Resonance 0…100% → Q 0.5…~16.
    const float fc = std::clamp(p.v[kCutoff] * v.cutoffScale, 10.0f, 0.45f * kSampleRate);
    const float g = std::tan(std::numbers::pi_v<float> * fc / kSampleRate);
    k = 2.0f - 1.94f * std::clamp(p.v[kReso], 0.0f, 1.0f);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  const float drive = p.v[kDrive];
  const float driveGain = 1.0f + 15.0f * drive * drive;
  const float makeup = 1.0f / std::sqrt(driveGain);

  const bool stereo = s.channels == 2;
  const float* L = s.channel(0);
  const float* R = stereo ? s.channel(1) : L;

  for (std::size_t i = 0; i < frames; ++i) {
    const auto idx = static_cast<int64_t>(std::floor(v.pos));
    if (idx < 0 || idx >= s.frames) {
      v.on = false;  // ran off the end (or the start, reversed)
      break;
    }
    const auto frac = static_cast<float>(v.pos - static_cast<double>(idx));
    float xl = hermite(L, idx, frac);
    float xr = stereo ? hermite(R, idx, frac) : xl;
    v.pos += rate;

    if (drive > 0) {
      xl = (1 - drive) * xl + drive * softClip(xl * driveGain) * makeup;
      if (stereo) xr = (1 - drive) * xr + drive * softClip(xr * driveGain) * makeup;
    }
    if (filter != kFilterOff) {
      auto svf = [&](float x, float& s1, float& s2) {
        const float v3 = x - s2;
        const float v1 = a1 * s1 + a2 * v3;
        const float v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2 * v1 - s1;
        s2 = 2 * v2 - s2;
        return filter == kLowPass ? v2 : filter == kBandPass ? k * v1 : x - k * v1 - v2;  // band-pass: unity at the centre
      };
      xl = svf(xl, v.s1L, v.s2L);
      xr = stereo ? svf(xr, v.s1R, v.s2R) : xl;
    }

    // Envelope, then a fade if the voice is being stopped.
    float e = v.amp.next(env);
    if (v.fading) {
      v.fade -= 1.0f / static_cast<float>(kFadeFrames);
      e *= std::max(0.0f, v.fade);
    }
    if (v.declick > 0) e *= 1.0f - static_cast<float>(v.declick--) / static_cast<float>(kDeclickFrames + 1);
    v.gainL += dl;
    v.gainR += dr;
    left[i] += xl * e * v.gainL;
    right[i] += xr * e * v.gainR;
    if (v.amp.idle() || (v.fading && v.fade <= 0)) {
      v.on = false;
      break;
    }
  }
  if (!std::isfinite(v.s1L + v.s2L + v.s1R + v.s2R)) v.s1L = v.s2L = v.s1R = v.s2R = 0;
}

}  // namespace ion
