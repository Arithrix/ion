#include "engine/module.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/denormals.h"
#include "engine/fm/fm_engine.h"

namespace ion {
namespace {

// −0.3 dBFS. A hard ceiling so nothing reaches the speakers above full scale.
// Phase 4 puts the master limiter in front of it.
constexpr float kCeiling = 0.9660509f;
// Meter decay per 64-frame block: about 20 dB per second.
constexpr float kPeakDecay = 0.99694f;
// RMS smoothing per block: ~300 ms.
constexpr float kRmsCoeff = 0.0044f;

constexpr uint8_t kCcAllNotesOff = 123;

struct PanGains {
  float left, right;
};

// Constant-power pan, unity at centre: hard left is +3 dB on the left, silent right.
PanGains panGains(float pan) {
  const float theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * std::numbers::pi_v<float> / 4.0f;
  return {std::numbers::sqrt2_v<float> * std::cos(theta), std::numbers::sqrt2_v<float> * std::sin(theta)};
}

void meter(const float* l, const float* r, std::size_t n, float& peakL, float& peakR, float& msL, float& msR,
           MeterStatus& out) {
  float pl = 0, pr = 0, sl = 0, sr = 0;
  for (std::size_t i = 0; i < n; ++i) {
    pl = std::max(pl, std::fabs(l[i]));
    pr = std::max(pr, std::fabs(r[i]));
    sl += l[i] * l[i];
    sr += r[i] * r[i];
  }
  peakL = std::max(pl, peakL * kPeakDecay);
  peakR = std::max(pr, peakR * kPeakDecay);
  msL += kRmsCoeff * (sl / static_cast<float>(n) - msL);
  msR += kRmsCoeff * (sr / static_cast<float>(n) - msR);
  out.peakLeft.store(peakL, std::memory_order_relaxed);
  out.peakRight.store(peakR, std::memory_order_relaxed);
  out.rmsLeft.store(std::sqrt(msL), std::memory_order_relaxed);
  out.rmsRight.store(std::sqrt(msR), std::memory_order_relaxed);
}

}  // namespace

Module::Module() : clock_(kSampleRate) {
  for (int t = 0; t < kTrackCount; ++t) channels_[static_cast<std::size_t>(t)] = t;
  for (int s = 0; s < ParamStore::kScopes; ++s)
    for (int i = 0; i < kMixerParamCount; ++i)
      values_[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)] = mixerParams()[static_cast<std::size_t>(i)].def;
  for (int t = 0; t < kTrackCount; ++t) {
    const PanGains g = panGains(0);
    strips_[static_cast<std::size_t>(t)].gainLeft = g.left;
    strips_[static_cast<std::size_t>(t)].gainRight = g.right;
  }
  master_.gainLeft = master_.gainRight = 1.0f;
}

Module::~Module() {
  for (Engine* e : engines_) delete e;
  while (auto n = notifications_.pop())
    if (n->type == Notification::Type::EngineRetired) delete n->engine;
  // Engines still in flight to the audio thread.
  while (auto c = commandQueue_.pop())
    if (c->type == Command::Type::SetEngine) delete c->engine;
}

bool Module::pushMidi(const MidiEvent& e) {
  if (midiQueue_.push(e)) return true;
  status_.droppedMidi.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool Module::pushCommand(const Command& c) { return commandQueue_.push(c); }

std::optional<Notification> Module::popNotification() { return notifications_.pop(); }

void Module::notify(const Notification& n) noexcept {
  // If the control thread has stopped draining, a retired engine leaks rather
  // than being deleted here: never free memory on the audio thread.
  notifications_.push(n);
}

// ---------------------------------------------------------------------------
// audio thread

void Module::process(float* out, std::size_t frames, int64_t cycleNs) noexcept {
  disableDenormals();
  const auto start = std::chrono::steady_clock::now();

  // Place newly arrived MIDI on the frame timeline. In Tight mode an event that
  // arrived Δ before this cycle started plays at (callback length − Δ) into it:
  // a constant one-buffer delay instead of jitter.
  const int64_t latency = timing_ == MidiTiming::Tight ? static_cast<int64_t>(frames) : 0;
  while (auto e = midiQueue_.pop()) {
    int64_t frame = -1;
    switch (e->timing) {
      case MidiEvent::Timing::Now: break;
      case MidiEvent::Timing::Frame: frame = e->time; break;
      case MidiEvent::Timing::Monotonic:
        if (cycleNs != 0) {
          frame = outFrame_ + latency + (e->time - cycleNs) * kSampleRate / 1'000'000'000;
          // Clock skew or a stale timestamp must never park an event in the
          // future (it would hold up everything behind it) or far in the past.
          frame = std::clamp(frame, outFrame_, outFrame_ + latency + static_cast<int64_t>(frames));
        }
        break;
    }
    if (pendingCount_ == pending_.size()) {
      status_.droppedMidi.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    pending_[(pendingHead_ + pendingCount_) % pending_.size()] = {e->msg, frame};
    ++pendingCount_;
  }

  std::size_t done = 0;
  while (done < frames) {
    if (blockPos_ == kBlockSize) {
      renderBlock();
      blockPos_ = 0;
    }
    const std::size_t n = std::min(frames - done, kBlockSize - blockPos_);
    for (std::size_t i = 0; i < n; ++i) {
      out[2 * (done + i)] = left_[blockPos_ + i];
      out[2 * (done + i) + 1] = right_[blockPos_ + i];
    }
    blockPos_ += n;
    done += n;
  }
  outFrame_ += static_cast<int64_t>(frames);

  const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - start;
  const float period = static_cast<float>(frames) / static_cast<float>(kSampleRate);
  const float load = period > 0 ? elapsed.count() / period : 0.0f;
  load_ += 0.05f * (load - load_);
  status_.dspLoad.store(load_, std::memory_order_relaxed);
  if (load > status_.dspLoadPeak.load(std::memory_order_relaxed))
    status_.dspLoadPeak.store(load, std::memory_order_relaxed);
  status_.callbackFrames.store(static_cast<uint32_t>(frames), std::memory_order_relaxed);
}

void Module::renderBlock() noexcept {
  while (auto c = commandQueue_.pop()) handleCommand(*c);

  clock_.update(frames_);
  FmEngine::setClock(static_cast<uint64_t>(frames_));

  for (int t = 0; t < kTrackCount; ++t) {
    std::memset(trackLeft_[t], 0, sizeof trackLeft_[t]);
    std::memset(trackRight_[t], 0, sizeof trackRight_[t]);
  }

  // Render engines, splitting the block at each MIDI event so it lands on its frame.
  const int64_t blockEnd = frames_ + static_cast<int64_t>(kBlockSize);
  std::size_t pos = 0;
  while (pendingCount_ > 0) {
    const Pending& p = pending_[pendingHead_];
    if (p.frame >= blockEnd) break;
    std::size_t offset = p.frame < 0 ? 0 : static_cast<std::size_t>(std::max<int64_t>(0, p.frame - frames_));
    offset = std::max(offset, pos);  // late events play now, never out of order
    if (offset > pos) {
      renderEngines(pos, offset);
      pos = offset;
    }
    dispatch(p.msg, frames_ + static_cast<int64_t>(offset));
    pendingHead_ = (pendingHead_ + 1) % pending_.size();
    --pendingCount_;
  }
  if (pos < kBlockSize) renderEngines(pos, kBlockSize);

  mix();

  frames_ = blockEnd;
  status_.framesRendered.store(static_cast<uint64_t>(frames_), std::memory_order_relaxed);
  status_.bpm.store(static_cast<float>(clock_.bpm()), std::memory_order_relaxed);
  status_.clockPresent.store(clock_.present(), std::memory_order_relaxed);
  status_.running.store(clock_.running(), std::memory_order_relaxed);
  status_.bar.store(clock_.bar(), std::memory_order_relaxed);
  status_.beat.store(clock_.beat(), std::memory_order_relaxed);
}

void Module::renderEngines(std::size_t from, std::size_t to) noexcept {
  for (int t = 0; t < kTrackCount; ++t)
    if (Engine* e = engines_[static_cast<std::size_t>(t)]) e->render(trackLeft_[t] + from, trackRight_[t] + from, to - from);
}

void Module::mix() noexcept {
  std::memset(left_, 0, sizeof left_);
  std::memset(right_, 0, sizeof right_);

  bool anySolo = false;
  for (int t = 0; t < kTrackCount; ++t) anySolo |= values_[static_cast<std::size_t>(t)][kSolo] >= 0.5f;

  int voices = 0;
  constexpr float kInvBlock = 1.0f / static_cast<float>(kBlockSize);
  for (int t = 0; t < kTrackCount; ++t) {
    const auto ti = static_cast<std::size_t>(t);
    const auto& v = values_[ti];
    StripState& s = strips_[ti];
    float* l = trackLeft_[t];
    float* r = trackRight_[t];

    if (engines_[ti]) eq_[ti].process(l, r, kBlockSize, EqSettings::fromParams(v.data()));

    const bool audible = v[kMute] < 0.5f && (!anySolo || v[kSolo] >= 0.5f);
    const float gain = audible ? faderGain(v[kFader]) : 0.0f;
    const PanGains pan = panGains(v[kPan]);
    const float targetL = gain * pan.left, targetR = gain * pan.right;
    const float stepL = (targetL - s.gainLeft) * kInvBlock, stepR = (targetR - s.gainRight) * kInvBlock;
    for (std::size_t i = 0; i < kBlockSize; ++i) {
      const float gl = s.gainLeft + stepL * static_cast<float>(i + 1);
      const float gr = s.gainRight + stepR * static_cast<float>(i + 1);
      l[i] *= gl;
      r[i] *= gr;
      left_[i] += l[i];
      right_[i] += r[i];
    }
    s.gainLeft = targetL;
    s.gainRight = targetR;
    meter(l, r, kBlockSize, s.peakLeft, s.peakRight, s.msLeft, s.msRight, status_.track[ti]);

    const int tv = engines_[ti] ? engines_[ti]->voiceCount() : 0;
    status_.trackVoices[ti].store(tv, std::memory_order_relaxed);
    voices += tv;
  }
  status_.voices.store(voices, std::memory_order_relaxed);

  // Master: fader, then the safety ceiling.
  const float mg = faderGain(values_[kMasterScope][kFader]);
  const float step = (mg - master_.gainLeft) * kInvBlock;
  bool clipped = false;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const float g = master_.gainLeft + step * static_cast<float>(i + 1);
    float& l = left_[i];
    float& r = right_[i];
    l *= g;
    r *= g;
    if (!std::isfinite(l)) l = 0;  // never let a NaN reach the speakers
    if (!std::isfinite(r)) r = 0;
    if (std::fabs(l) > kCeiling || std::fabs(r) > kCeiling) clipped = true;
    l = std::clamp(l, -kCeiling, kCeiling);
    r = std::clamp(r, -kCeiling, kCeiling);
  }
  master_.gainLeft = master_.gainRight = mg;
  if (clipped) status_.clippedBlocks.fetch_add(1, std::memory_order_relaxed);
  meter(left_, right_, kBlockSize, master_.peakLeft, master_.peakRight, master_.msLeft, master_.msRight,
        status_.master);
}

void Module::dispatch(const MidiMessage& m, int64_t frame) noexcept {
  status_.midiReceived.fetch_add(1, std::memory_order_relaxed);

  switch (m.status) {
    case 0xF8: clock_.tick(frame); return;
    case 0xFA: clock_.start(frame); return;
    case 0xFB: clock_.cont(frame); return;
    case 0xFC: clock_.stop(frame); return;
    case 0xF2: clock_.songPosition(m.data1 | (m.data2 << 7)); return;
    default: break;
  }
  status_.lastMidi.store(m.status | (m.data1 << 8) | (m.data2 << 16), std::memory_order_relaxed);
  if (m.status >= 0xF0) return;

  const uint8_t type = m.type();
  const uint8_t ch = m.channel();

  if (type == 0xB0) {
    if (learn_) {
      notify({Notification::Type::CcLearned, nullptr, ch, m.data1, *learn_});
      learn_.reset();
      status_.learning.store(false, std::memory_order_relaxed);
      return;
    }
    bool mapped = false;
    for (const CcMapping& map : ccMap_) {
      if (!map.used || map.cc != m.data1 || (map.channel != kAnyChannel && map.channel != ch)) continue;
      if (const ParamInfo* pi = info(map.target)) {
        const float n = map.lo + (map.hi - map.lo) * (static_cast<float>(m.data2) / 127.0f);
        const float value = (pi->curve == Curve::Toggle) ? (m.data2 >= 64 ? 1.0f : 0.0f) : fromNormalized(*pi, n);
        applyParam(map.target, value);
        mapped = true;
      }
    }
    if (mapped && m.data1 != kCcAllNotesOff) return;
  }

  for (int t = 0; t < kTrackCount; ++t) {
    const auto ti = static_cast<std::size_t>(t);
    Engine* e = engines_[ti];
    if (!e || channels_[ti] != ch) continue;
    switch (type) {
      case 0x90:
        if (m.data2 > 0) {
          e->noteOn(m.data1, m.data2);
          status_.trackNotes[ti].fetch_add(1, std::memory_order_relaxed);
        } else {
          e->noteOff(m.data1);
        }
        break;
      case 0x80: e->noteOff(m.data1); break;
      case 0xB0: e->controlChange(m.data1, m.data2); break;
      case 0xE0: e->pitchBend(((m.data2 << 7) | m.data1) - 8192); break;
      default: break;
    }
  }
}

const ParamInfo* Module::info(ParamRef ref) const noexcept {
  if (ref.scope >= ParamStore::kScopes) return nullptr;
  if (!ref.isEngine()) return ref.index < kMixerParamCount ? &mixerParams()[ref.index] : nullptr;
  if (ref.scope >= kTrackCount) return nullptr;
  const Engine* e = engines_[ref.scope];
  if (!e) return nullptr;
  const auto table = engineParams(e->type());
  const std::size_t i = ref.index - kEngineParamBase;
  return i < table.size() ? &table[i] : nullptr;
}

void Module::applyParam(ParamRef ref, float value) noexcept {
  const ParamInfo* pi = info(ref);
  if (!pi) return;
  value = clampValue(*pi, value);
  values_[ref.scope][ref.index] = value;
  store_.publish(ref, value);
  if (ref.isEngine()) engines_[ref.scope]->setParam(ref.index - kEngineParamBase, value);
}

void Module::handleCommand(const Command& c) noexcept {
  const auto track = static_cast<std::size_t>(std::min<int>(c.track, kTrackCount - 1));
  switch (c.type) {
    case Command::Type::Midi:
      dispatch(c.midi, frames_);
      break;
    case Command::Type::Panic:
      for (Engine* e : engines_)
        if (e) e->allNotesOff();
      break;
    case Command::Type::LoadVoice:
      if (Engine* e = engines_[track]; e && e->type() == EngineType::Fm) static_cast<FmEngine*>(e)->loadVoice(c.voice);
      break;
    case Command::Type::SetParam:
      applyParam(c.param, c.value);
      break;
    case Command::Type::SetEngine: {
      Engine* old = engines_[track];
      engines_[track] = c.engine;
      status_.trackEngine[track].store(static_cast<uint8_t>(c.engine ? c.engine->type() : EngineType::None),
                                       std::memory_order_relaxed);
      // A new engine starts at its defaults; the control thread follows up
      // with SetParam for any saved values.
      if (c.engine) {
        const auto table = engineParams(c.engine->type());
        for (std::size_t i = 0; i < table.size(); ++i) {
          const ParamRef ref{static_cast<uint8_t>(track), static_cast<uint8_t>(kEngineParamBase + i)};
          values_[track][ref.index] = table[i].def;
          store_.publish(ref, table[i].def);
        }
      }
      if (old) notify({Notification::Type::EngineRetired, old, 0, 0, {}});
      break;
    }
    case Command::Type::SetChannel:
      channels_[track] = std::clamp(static_cast<int>(c.value), 0, 15);
      break;
    case Command::Type::SetCcMapping:
      if (c.slot >= 0 && c.slot < kMaxCcMappings) ccMap_[static_cast<std::size_t>(c.slot)] = c.mapping;
      break;
    case Command::Type::LearnCc:
      learn_ = c.param;
      status_.learning.store(true, std::memory_order_relaxed);
      break;
    case Command::Type::CancelLearn:
      learn_.reset();
      status_.learning.store(false, std::memory_order_relaxed);
      break;
    case Command::Type::SetTiming:
      timing_ = c.value >= 0.5f ? MidiTiming::Immediate : MidiTiming::Tight;
      break;
  }
}

}  // namespace ion
