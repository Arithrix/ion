#include "engine/fm/fm_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <dexed.h>

uint32_t g_ion_dexed_millis = 0;

namespace ion {

// Synth_Dexed renders through a protected method; it expects to be subclassed.
class DexedCore : public Dexed {
 public:
  using Dexed::Dexed;
  using Dexed::getSamples;

  // loadVoiceParameters() stops every voice, then flags a "refresh" for live
  // notes. Nothing is live after the load, and a refresh landing on a note
  // started before the next render would re-run its envelope update, so it's
  // cleared.
  void loadVoice(const syx::Voice& voice) {
    syx::Voice copy = voice;  // Dexed takes a non-const pointer
    loadVoiceParameters(copy.data());
    refreshVoice = false;
  }

  // Changes the voice under live notes (macros). The refresh re-targets each
  // held note's envelopes on the next render; see the ion change in env.cpp.
  void updateVoice(const syx::Voice& voice, bool notesLive) {
    std::memcpy(data, voice.data(), voice.size());  // data[155], the operator mask, stays
    if (notesLive) refreshVoice = true;
  }

  syx::Voice voice() const {
    syx::Voice v;
    std::memcpy(v.data(), data, v.size());
    return v;
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
    {"bright", "Bright", -50, 50, 0, Curve::Choice, Unit::Offset, 1, 5},
    {"attack", "Attack", -50, 50, 0, Curve::Choice, Unit::Offset, 1, 5},
    {"release", "Release", -50, 50, 0, Curve::Choice, Unit::Offset, 1, 5},
    {"feedback", "Feedback", -7, 7, 0, Curve::Choice, Unit::Offset, 1, 2},
    {"detune", "Detune", -7, 7, 0, Curve::Choice, Unit::Offset, 1, 2},
    {"velocity", "Velocity", -7, 7, 0, Curve::Choice, Unit::Offset, 1, 2},
}};

}  // namespace

std::span<const ParamInfo> FmEngine::paramTable() { return kParams; }

FmEngine::FmEngine() {
  owner_.fill(-1);
  for (Slot& s : slots_) {
    s.dexed = std::make_unique<DexedCore>(static_cast<uint8_t>(kMaxPolyphony), static_cast<uint32_t>(kSampleRate));
    s.dexed->setEngineType(MKI);  // Dexed's "Mark I" mode: closest to the original hardware
    s.patch = s.dexed->voice();   // Dexed's init voice
  }
  for (int i = 0; i < kParamCount; ++i) setParam(i, kParams[static_cast<std::size_t>(i)].def);
  for (Slot& s : slots_) s.dexed->activate();
}

FmEngine::~FmEngine() = default;

void FmEngine::each(auto&& fn) {
  for (Slot& s : slots_) fn(*s.dexed);
}

void FmEngine::applyMacros(Slot& s) { s.dexed->updateVoice(dx7::applyMacros(s.patch, macros_), s.sounding); }

void FmEngine::syncControllers(Slot& s) {
  DexedCore& d = *s.dexed;
  d.setPitchbend(static_cast<int16_t>(pitchBend_));
  d.setModWheel(modWheel_);
  d.setBreathController(breath_);
  d.setFootController(foot_);
  d.ControllersRefresh();
  d.setSustain(sustain_);
}

void FmEngine::stop(Slot& s) {
  s.dexed->panic();
  // The DC blocker still holds the last (unfaded) output; left in place it
  // would come out as a step when this instance next plays.
  s.dexed->resetFxState();
  s.sounding = false;
  s.fade = 0;
  s.quiet = 0;
  const auto index = static_cast<int8_t>(&s - slots_.data());
  for (int8_t& o : owner_)
    if (o == index) o = -1;
}

void FmEngine::loadVoice(const syx::Voice& voice) {
  // The spare: a silent instance other than the active one. If both others are
  // still sounding, take the one that's fading out (or the older tail) and cut it.
  int spare = -1;
  for (int i = 0; i < kSlots; ++i)
    if (i != active_ && !slots_[static_cast<std::size_t>(i)].sounding) spare = i;
  if (spare < 0) {
    for (int i = 0; i < kSlots; ++i)
      if (i != active_ && (spare < 0 || slots_[static_cast<std::size_t>(i)].fade > 0)) spare = i;
    stop(slots_[static_cast<std::size_t>(spare)]);
  }
  // Whatever else is still sounding (the tail from the last change) fades out;
  // the instance that was active becomes the new tail and finishes its notes.
  for (int i = 0; i < kSlots; ++i) {
    Slot& s = slots_[static_cast<std::size_t>(i)];
    if (i != active_ && i != spare && s.sounding && s.fade == 0) s.fade = kFadeFrames;
  }

  Slot& s = slots_[static_cast<std::size_t>(spare)];
  s.patch = voice;
  s.dexed->loadVoice(dx7::applyMacros(voice, macros_));
  syncControllers(s);
  active_ = spare;
}

syx::Voice FmEngine::effectiveVoice() const {
  return dx7::applyMacros(slots_[static_cast<std::size_t>(active_)].patch, macros_);
}

int FmEngine::soundingSlots() const {
  return static_cast<int>(std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.sounding; }));
}

void FmEngine::noteOn(uint8_t note, uint8_t velocity) {
  note &= 0x7F;
  // A note still held on an older instance is released there first, or its
  // note-off would never reach it.
  if (const int8_t o = owner_[note]; o >= 0 && o != active_) slots_[static_cast<std::size_t>(o)].dexed->keyup(note);
  if (mono_) {
    // One note at a time, across instances too.
    for (int n = 0; n < 128; ++n) {
      const int8_t o = owner_[static_cast<std::size_t>(n)];
      if (o >= 0 && o != active_) {
        slots_[static_cast<std::size_t>(o)].dexed->keyup(static_cast<uint8_t>(n));
        owner_[static_cast<std::size_t>(n)] = -1;
      }
    }
  }
  Slot& s = slots_[static_cast<std::size_t>(active_)];
  s.dexed->keydown(note, velocity);
  s.sounding = true;
  s.quiet = 0;
  owner_[note] = static_cast<int8_t>(active_);
}

void FmEngine::noteOff(uint8_t note) {
  note &= 0x7F;
  const int8_t o = owner_[note];
  slots_[static_cast<std::size_t>(o >= 0 ? o : active_)].dexed->keyup(note);
  owner_[note] = -1;
}

void FmEngine::controlChange(uint8_t controller, uint8_t value) {
  switch (controller) {
    case kCcModWheel: modWheel_ = value; each([&](DexedCore& d) { d.setModWheel(value); d.ControllersRefresh(); }); break;
    case kCcBreath: breath_ = value; each([&](DexedCore& d) { d.setBreathController(value); d.ControllersRefresh(); }); break;
    case kCcFoot: foot_ = value; each([&](DexedCore& d) { d.setFootController(value); d.ControllersRefresh(); }); break;
    case kCcSustain: sustain_ = value >= 64; each([&](DexedCore& d) { d.setSustain(sustain_); }); break;
    case kCcAllSoundOff: allNotesOff(); break;
    case kCcAllNotesOff: each([](DexedCore& d) { d.notesOff(); }); break;
    default: break;
  }
}

void FmEngine::pitchBend(int value) {
  pitchBend_ = value;
  each([&](DexedCore& d) { d.setPitchbend(static_cast<int16_t>(value)); });
}

void FmEngine::allNotesOff() {
  for (Slot& s : slots_) stop(s);
  sustain_ = false;
}

void FmEngine::setParam(int index, float value) {
  const int v = static_cast<int>(std::lround(value));
  int* macro = nullptr;
  switch (index) {
    case kPoly: each([&](DexedCore& d) { d.setMaxNotes(static_cast<uint8_t>(v)); }); return;  // stops held notes
    case kBendRange: each([&](DexedCore& d) { d.setPitchbendRange(static_cast<uint8_t>(v)); }); return;
    case kMono: mono_ = v != 0; each([&](DexedCore& d) { d.setMonoMode(mono_); }); return;
    case kBright: macro = &macros_.bright; break;
    case kAttack: macro = &macros_.attack; break;
    case kRelease: macro = &macros_.release; break;
    case kFeedback: macro = &macros_.feedback; break;
    case kDetune: macro = &macros_.detune; break;
    case kVelocity: macro = &macros_.velocity; break;
    default: return;
  }
  if (*macro == v) return;
  *macro = v;
  // Every instance: held notes on the tail follow the macros too.
  for (Slot& s : slots_) applyMacros(s);
}

void FmEngine::render(float* left, float* right, std::size_t frames) {
  std::size_t done = 0;
  while (done < frames) {
    if (pos_ == kBlockSize) {
      std::fill(std::begin(mix_), std::end(mix_), 0.0f);
      voices_ = 0;
      for (Slot& s : slots_) {
        if (!s.sounding) continue;
        s.dexed->getSamples(scratch_, static_cast<uint16_t>(kBlockSize));
        if (s.fade > 0) {
          for (std::size_t i = 0; i < kBlockSize; ++i) {
            const float g = static_cast<float>(std::max(0, s.fade - static_cast<int>(i))) / kFadeFrames;
            mix_[i] += scratch_[i] * g;
          }
          s.fade -= static_cast<int>(kBlockSize);
          if (s.fade <= 0) {
            stop(s);
            continue;
          }
        } else {
          for (std::size_t i = 0; i < kBlockSize; ++i) mix_[i] += scratch_[i];
        }
        // Also retires voices whose carriers have gone silent; without this
        // call Synth_Dexed keeps computing finished notes.
        const int n = s.dexed->getNumNotesPlaying();
        voices_ += n;
        s.quiet = n > 0 ? 0 : s.quiet + static_cast<int>(kBlockSize);
        if (s.quiet >= kQuietFrames) {
          s.sounding = false;
          s.quiet = 0;
          s.dexed->resetFxState();  // what's left is far below audibility
        }
      }
      pos_ = 0;
    }
    const std::size_t n = std::min(frames - done, kBlockSize - pos_);
    for (std::size_t i = 0; i < n; ++i) {
      const float s = mix_[pos_ + i] * kOutputGain;
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

}  // namespace ion
