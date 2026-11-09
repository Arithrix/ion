#include "engine/module.h"

#include "engine/plaits/plaits_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

#include "core/denormals.h"
#include "engine/drum/drum_engine.h"
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
  for (int s = 0; s < kScopeCount; ++s) {
    const PanGains g = panGains(0);
    strips_[static_cast<std::size_t>(s)].gainLeft = g.left;
    strips_[static_cast<std::size_t>(s)].gainRight = g.right;
  }
  master_.gainLeft = master_.gainRight = 1.0f;
  // The send buses' own effects: wet only, always in.
  for (const auto& [scope, type] : {std::pair{kDelayScope, EffectType::Delay}, std::pair{kReverbScope, EffectType::Reverb}}) {
    Effect* e = createEffect(type).release();
    e->setWetOnly(true);
    fx_[scope][0] = e;
    fxMix_[scope][0] = 1;
    const ParamRef on = insertOnRef(scope, 0);
    values_[scope][on.index] = 1;
    store_.publish(on, 1);
    const auto table = effectParams(type);
    for (std::size_t i = 0; i < table.size(); ++i) {
      const ParamRef ref = insertParamRef(scope, 0, static_cast<int>(i));
      values_[scope][ref.index] = table[i].def;
      store_.publish(ref, table[i].def);
    }
  }
}

Module::~Module() {
  for (Engine* e : engines_) delete e;
  for (auto& scope : fx_)
    for (Effect* e : scope) delete e;
  delete programs_;
  // Samples belong to the Session, which frees them itself.
  while (auto n = notifications_.pop()) {
    if (n->type == Notification::Type::EngineRetired) delete n->engine;
    if (n->type == Notification::Type::ProgramsRetired) delete n->programs;
    if (n->type == Notification::Type::EffectRetired) delete n->effect;
  }
  // Engines and tables still in flight to the audio thread.
  while (auto c = commandQueue_.pop()) {
    if (c->type == Command::Type::SetEngine) delete c->engine;
    if (c->type == Command::Type::SetPrograms) delete c->programs;
    if (c->type == Command::Type::SetInsert) delete c->effect;
  }
}

bool Module::pushMidi(const MidiEvent& e) {
  if (midiQueue_.push(e)) return true;
  status_.droppedMidi.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool Module::pushCommand(const Command& c) { return commandQueue_.push(c); }

std::optional<Notification> Module::popNotification() { return notifications_.pop(); }

void Module::releaseSample(const Sample* s) noexcept {
  if (!s) return;
  Notification n{Notification::Type::SampleReleased};
  n.sample = s;
  notify(n);
}

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
  if (!clock_.present()) takeClockStopped();
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
  for (Engine* e : engines_)
    if (e)
      for (const Sample* s; e->popReleased(s);) releaseSample(s);

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

void Module::runInserts(int scope, float* l, float* r) noexcept {
  const auto si = static_cast<std::size_t>(scope);
  for (int slot = 0; slot < kInsertSlots; ++slot) {
    const auto k = static_cast<std::size_t>(slot);
    Effect* e = fx_[si][k];
    if (!e) continue;
    // A return's own effect is the return: it can't be bypassed.
    const bool fixed = scope >= kDelayScope && slot == 0;
    const float target = fixed || values_[si][insertOnRef(scope, slot).index] >= 0.5f ? 1.0f : 0.0f;
    float& m = fxMix_[si][k];
    if (m == 0 && target == 0) {
      status_.reduction[si][k].store(0, std::memory_order_relaxed);
      continue;  // bypassed: costs nothing
    }
    if (m == 0) e->reset();  // coming back in: no stale tail
    if (m == 1 && target == 1) {
      e->process(l, r, kBlockSize);
    } else {
      // Switching in or out: crossfade over this block.
      std::memcpy(scratchLeft_, l, sizeof scratchLeft_);
      std::memcpy(scratchRight_, r, sizeof scratchRight_);
      e->process(scratchLeft_, scratchRight_, kBlockSize);
      const float step = (target - m) / static_cast<float>(kBlockSize);
      for (std::size_t i = 0; i < kBlockSize; ++i) {
        const float w = m + step * static_cast<float>(i + 1);
        l[i] += (scratchLeft_[i] - l[i]) * w;
        r[i] += (scratchRight_[i] - r[i]) * w;
      }
      m = target;
    }
    status_.reduction[si][k].store(e->reduction(), std::memory_order_relaxed);
  }
}

namespace {
constexpr int kSleepAfter = 8;          // blocks of silence (about 10 ms)
constexpr float kSilence = 1e-7f;       // −140 dB
float blockPeak(const float* l, const float* r) {
  float p = 0;
  for (std::size_t i = 0; i < kBlockSize; ++i) p = std::max(p, std::max(std::fabs(l[i]), std::fabs(r[i])));
  return p;
}
}  // namespace

// True when the scope's EQ and inserts can be skipped for this block: nothing
// coming in, and what came out has died away. Anything arriving wakes it.
bool Module::asleep(int scope, const float* l, const float* r) noexcept {
  int& q = quiet_[static_cast<std::size_t>(scope)];
  if (blockPeak(l, r) > 0) {
    q = 0;
    return false;
  }
  int needed = kSleepAfter;
  for (const Effect* e : fx_[static_cast<std::size_t>(scope)])
    if (e) needed = std::max(needed, kSleepAfter + e->memoryBlocks());
  return q >= needed;
}

// After processing: count a block that went out (near) silent.
void Module::settle(int scope, const float* l, const float* r) noexcept {
  int& q = quiet_[static_cast<std::size_t>(scope)];
  q = blockPeak(l, r) < kSilence ? std::min(q + 1, 1 << 20) : 0;
}

void Module::strip(int scope, float* l, float* r, float gainScale, float* outL, float* outR) noexcept {
  const auto si = static_cast<std::size_t>(scope);
  const auto& v = values_[si];
  StripState& s = strips_[si];
  const float gain = gainScale * faderGain(v[kFader]);
  const PanGains pan = panGains(v[kPan]);
  constexpr float kInvBlock = 1.0f / static_cast<float>(kBlockSize);
  const float targetL = gain * pan.left, targetR = gain * pan.right;
  const float stepL = (targetL - s.gainLeft) * kInvBlock, stepR = (targetR - s.gainRight) * kInvBlock;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    l[i] *= s.gainLeft + stepL * static_cast<float>(i + 1);
    r[i] *= s.gainRight + stepR * static_cast<float>(i + 1);
    outL[i] += l[i];
    outR[i] += r[i];
  }
  s.gainLeft = targetL;
  s.gainRight = targetR;
}

void Module::mix() noexcept {
  std::memset(left_, 0, sizeof left_);
  std::memset(right_, 0, sizeof right_);
  std::memset(busLeft_, 0, sizeof busLeft_);
  std::memset(busRight_, 0, sizeof busRight_);

  // Tempo for synced effects: the OXI's clock, or 120 BPM without one.
  const double bpm = clock_.present() ? clock_.bpm() : 120.0;
  for (auto& scope : fx_)
    for (Effect* e : scope)
      if (e) e->setTempo(bpm);
  for (Engine* e : engines_)
    if (e) e->setTempo(bpm);

  bool anySolo = false;
  for (int t = 0; t < kTrackCount; ++t) anySolo |= values_[static_cast<std::size_t>(t)][kSolo] >= 0.5f;

  int voices = 0;
  constexpr float kInvBlock = 1.0f / static_cast<float>(kBlockSize);
  const bool recording = capturing();
  if (recording) {
    std::memset(recLeft_, 0, sizeof recLeft_);
    std::memset(recRight_, 0, sizeof recRight_);
  }
  for (int t = 0; t < kTrackCount; ++t) {
    const auto ti = static_cast<std::size_t>(t);
    const auto& v = values_[ti];
    StripState& s = strips_[ti];
    float* l = trackLeft_[t];
    float* r = trackRight_[t];
    const bool rec = recording && take_->source == RecSource::Tracks && (take_->tracks >> t & 1);
    if (rec && take_->tap == RecTap::Pre)
      for (std::size_t i = 0; i < kBlockSize; ++i) {
        recLeft_[i] += l[i];
        recRight_[i] += r[i];
      }

    if (!asleep(t, l, r)) {
      if (engines_[ti]) eq_[ti].process(l, r, kBlockSize, EqSettings::fromParams(v.data()));
      runInserts(t, l, r);
      settle(t, l, r);
    }

    if (rec && take_->tap == RecTap::Post) {
      // What the track sends to the master, but heard even when muted or soloed out.
      const PanGains rp = panGains(v[kPan]);
      const float g = faderGain(v[kFader]);
      float& gl = recGainLeft_[ti];
      float& gr = recGainRight_[ti];
      const float sl = (g * rp.left - gl) * kInvBlock, sr = (g * rp.right - gr) * kInvBlock;
      for (std::size_t i = 0; i < kBlockSize; ++i) {
        const float k = static_cast<float>(i + 1);
        recLeft_[i] += l[i] * (gl + sl * k);
        recRight_[i] += r[i] * (gr + sr * k);
      }
      gl = g * rp.left;
      gr = g * rp.right;
    }

    // Fader and pan into the master; sends to the buses, before or after the
    // fader. Mute (and solo elsewhere) silences the sends too.
    const bool audible = v[kMute] < 0.5f && (!anySolo || v[kSolo] >= 0.5f);
    const float gain = audible ? faderGain(v[kFader]) : 0.0f;
    const PanGains pan = panGains(v[kPan]);
    const float targetL = gain * pan.left, targetR = gain * pan.right;
    const float stepL = (targetL - s.gainLeft) * kInvBlock, stepR = (targetR - s.gainRight) * kInvBlock;
    const float sendA = audible ? faderGain(v[kSendA]) : 0.0f, sendB = audible ? faderGain(v[kSendB]) : 0.0f;
    const float stepA = (sendA - s.sendA) * kInvBlock, stepB = (sendB - s.sendB) * kInvBlock;
    const bool pre = v[kSendPre] >= 0.5f;
    const bool sends = sendA > 0 || sendB > 0 || s.sendA > 0 || s.sendB > 0;
    for (std::size_t i = 0; i < kBlockSize; ++i) {
      const float k = static_cast<float>(i + 1);
      const float dryL = l[i], dryR = r[i];
      l[i] *= s.gainLeft + stepL * k;
      r[i] *= s.gainRight + stepR * k;
      left_[i] += l[i];
      right_[i] += r[i];
      if (sends) {
        const float srcL = pre ? dryL : l[i], srcR = pre ? dryR : r[i];
        const float ga = s.sendA + stepA * k, gb = s.sendB + stepB * k;
        busLeft_[0][i] += srcL * ga;
        busRight_[0][i] += srcR * ga;
        busLeft_[1][i] += srcL * gb;
        busRight_[1][i] += srcR * gb;
      }
    }
    s.gainLeft = targetL;
    s.gainRight = targetR;
    s.sendA = sendA;
    s.sendB = sendB;
    meter(l, r, kBlockSize, s.peakLeft, s.peakRight, s.msLeft, s.msRight, status_.track[ti]);

    const int tv = engines_[ti] ? engines_[ti]->voiceCount() : 0;
    status_.trackVoices[ti].store(tv, std::memory_order_relaxed);
    voices += tv;
  }
  status_.voices.store(voices, std::memory_order_relaxed);

  // The returns. Solo doesn't silence them (they're solo-safe); mute does.
  // The delay return can feed the reverb (its Send B, after its fader).
  for (int bus = 0; bus < 2; ++bus) {
    const int scope = bus == 0 ? kDelayScope : kReverbScope;
    const auto si = static_cast<std::size_t>(scope);
    float* l = busLeft_[bus];
    float* r = busRight_[bus];
    if (!asleep(scope, l, r)) {
      runInserts(scope, l, r);
      eq_[si].process(l, r, kBlockSize, EqSettings::fromParams(values_[si].data()));
      settle(scope, l, r);
    }
    StripState& s = strips_[si];
    strip(scope, l, r, values_[si][kMute] >= 0.5f ? 0.0f : 1.0f, left_, right_);
    if (bus == 0) {
      const float sendB = values_[si][kMute] >= 0.5f ? 0.0f : faderGain(values_[si][kSendB]);
      const float step = (sendB - s.sendB) * kInvBlock;
      for (std::size_t i = 0; i < kBlockSize; ++i) {
        const float g = s.sendB + step * static_cast<float>(i + 1);
        busLeft_[1][i] += l[i] * g;
        busRight_[1][i] += r[i] * g;
      }
      s.sendB = sendB;
    }
    meter(l, r, kBlockSize, s.peakLeft, s.peakRight, s.msLeft, s.msRight, status_.returns[static_cast<std::size_t>(bus)]);
  }

  // Master: EQ → inserts (the glue compressor) → fader → limiter.
  if (!asleep(kMasterScope, left_, right_)) {
    eq_[kMasterScope].process(left_, right_, kBlockSize, EqSettings::fromParams(values_[kMasterScope].data()));
    runInserts(kMasterScope, left_, right_);
    settle(kMasterScope, left_, right_);
  }
  // The browser's preview isn't part of a recording of the master: it waits.
  if (!(recording && take_->source == RecSource::Master)) renderPreview();
  const float mg = faderGain(values_[kMasterScope][kFader]);
  const float step = (mg - master_.gainLeft) * kInvBlock;
  bool clipped = false;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const float g = master_.gainLeft + step * static_cast<float>(i + 1);
    float& l = left_[i];
    float& r = right_[i];
    l *= g;
    r *= g;
    if (!std::isfinite(l)) l = 0;  // never let a NaN reach the limiter or the speakers
    if (!std::isfinite(r)) r = 0;
    if (std::fabs(l) > kCeiling || std::fabs(r) > kCeiling) clipped = true;
  }
  master_.gainLeft = master_.gainRight = mg;
  limiter_.process(left_, right_, kBlockSize);
  status_.limiter.store(limiter_.reduction(), std::memory_order_relaxed);
  if (clipped) status_.clippedBlocks.fetch_add(1, std::memory_order_relaxed);
  meter(left_, right_, kBlockSize, master_.peakLeft, master_.peakRight, master_.msLeft, master_.msRight,
        status_.master);

  if (recording) {
    // The master comes out of the limiter 1 ms late: read it that much later, so
    // a take of the master lines up with the bar exactly as a take of tracks does.
    if (take_->source == RecSource::Master) captureTake(left_, right_, Limiter::kLookahead);
    else captureTake(recLeft_, recRight_, 0);
  }
}

// ---------------------------------------------------------------------------
// recording (DECISIONS D-51)

namespace {
// A synced take records this far past its last downbeat, so the control thread
// can trim to the length the clock measured over the whole take.
constexpr int64_t kTakeTail = 2048;
constexpr int kTicksPerBar = 96;
}  // namespace

void Module::startTake(Take* t) noexcept {
  if (take_) endTake(Take::State::Cancelled);
  take_ = t;
  takeEnd_ = -1;
  takeTicks_ = 0;
  recGainLeft_.fill(0);
  recGainRight_.fill(0);
  for (int tr = 0; tr < kTrackCount; ++tr) {
    // Start the post-fx tap at the track's gain, not ramping up from zero.
    const auto& v = values_[static_cast<std::size_t>(tr)];
    const PanGains p = panGains(v[kPan]);
    recGainLeft_[static_cast<std::size_t>(tr)] = faderGain(v[kFader]) * p.left;
    recGainRight_[static_cast<std::size_t>(tr)] = faderGain(v[kFader]) * p.right;
  }
  t->frames.store(0, std::memory_order_relaxed);
  t->tickCount.store(0, std::memory_order_relaxed);
  t->cutShort = false;
  t->bpm = clock_.present() && clock_.bpm() > 0 ? clock_.bpm() : 120.0;
  if (t->synced) {
    // Bars it may run to; a free synced take stops on the downbeat after "stop".
    const int room = static_cast<int>(std::max<std::size_t>(t->ticks.size(), 1) - 1) / kTicksPerBar;
    t->targetBars.store(t->bars > 0 ? std::min(t->bars, room) : room, std::memory_order_relaxed);
    t->state.store(Take::State::Armed, std::memory_order_release);
    return;
  }
  // Now: from this block. A length in bars is counted at the current tempo.
  takeStart_ = frames_;
  t->startFrame = frames_;
  t->startBar = 0;
  t->targetBars.store(t->bars, std::memory_order_relaxed);
  if (t->bars > 0) takeEnd_ = frames_ + std::llround(t->bars * 4 * 60.0 / t->bpm * kSampleRate);
  t->state.store(Take::State::Recording, std::memory_order_release);
}

void Module::takeTick(int64_t frame, bool downbeat) noexcept {
  Take* t = take_;
  if (!t || !t->synced) return;
  const Take::State st = t->state.load(std::memory_order_relaxed);
  if (st == Take::State::Armed) {
    if (!downbeat) return;
    takeStart_ = frame;
    takeTicks_ = 0;
    t->startFrame = frame;
    t->startBar = clock_.bar();  // the tick has been counted: this is the bar it opens
    t->bpm = clock_.bpm() > 0 ? clock_.bpm() : t->bpm;
    if (!t->ticks.empty()) t->ticks[0] = 0;
    t->tickCount.store(1, std::memory_order_relaxed);
    t->state.store(Take::State::Recording, std::memory_order_release);
    return;
  }
  if (st != Take::State::Recording || takeEnd_ >= 0) return;
  ++takeTicks_;
  if (static_cast<std::size_t>(takeTicks_) < t->ticks.size()) {
    t->ticks[static_cast<std::size_t>(takeTicks_)] = frame - takeStart_;
    t->tickCount.store(takeTicks_ + 1, std::memory_order_release);
  }
  if (takeTicks_ >= t->targetBars.load(std::memory_order_relaxed) * kTicksPerBar) takeEnd_ = frame + kTakeTail;
}

// The transport stopped or the clock went away: a synced take keeps the whole
// bars it has (the control thread drops it if there are none).
void Module::takeClockStopped() noexcept {
  if (!take_ || !take_->synced) return;
  const Take::State st = take_->state.load(std::memory_order_relaxed);
  if (st != Take::State::Recording || takeEnd_ >= 0) return;
  take_->targetBars.store(takeTicks_ / kTicksPerBar, std::memory_order_relaxed);
  take_->cutShort = true;
  takeEnd_ = frames_;  // the capture ends it
}

void Module::captureTake(const float* l, const float* r, int64_t delay) noexcept {
  Take* t = take_;
  const int64_t blockEnd = frames_ + static_cast<int64_t>(kBlockSize);
  const int64_t start = takeStart_ + delay, end = takeEnd_ >= 0 ? takeEnd_ + delay : -1;
  const int64_t from = std::max(start, frames_);
  int64_t to = end >= 0 ? std::min(end, blockEnd) : blockEnd;
  bool full = false;
  if (to - start > t->capacity) {
    to = start + t->capacity;
    full = true;
  }
  for (int64_t f = from; f < to; ++f) {
    const auto i = static_cast<std::size_t>(f - start);
    const auto k = static_cast<std::size_t>(f - frames_);
    t->left[i] = l[k];
    t->right[i] = r[k];
  }
  if (to > from) t->frames.store(to - start, std::memory_order_release);
  if (full && end < 0) t->cutShort = true;
  if (full || (end >= 0 && blockEnd >= end)) endTake(Take::State::Done);
}

void Module::endTake(Take::State state) noexcept {
  Take* t = std::exchange(take_, nullptr);
  if (!t) return;
  takeEnd_ = -1;
  t->state.store(state, std::memory_order_release);
  Notification n{Notification::Type::TakeDone};
  n.take = t;
  notify(n);
}

void Module::renderPreview() noexcept {
  constexpr float kFadeStep = 1.0f / 240.0f;  // 5 ms
  bool any = false;
  for (PreviewVoice& p : preview_) {
    if (!p.sample) continue;
    const Sample& s = *p.sample;
    const float* l = s.channel(0);
    const float* r = s.channel(s.channels > 1 ? 1 : 0);
    std::size_t i = 0;
    for (; i < kBlockSize && p.pos < s.frames; ++i, ++p.pos) {
      if (p.fading) p.fade = std::max(0.0f, p.fade - kFadeStep);
      left_[i] += l[p.pos] * p.gain * p.fade;
      right_[i] += r[p.pos] * p.gain * p.fade;
    }
    if (p.pos >= s.frames || (p.fading && p.fade <= 0)) {
      releaseSample(p.sample);
      p = {};
    } else {
      any = true;
    }
  }
  status_.previewing.store(any, std::memory_order_relaxed);
}

void Module::dispatch(const MidiMessage& m, int64_t frame) noexcept {
  status_.midiReceived.fetch_add(1, std::memory_order_relaxed);

  switch (m.status) {
    case 0xF8: {
      // A downbeat is a tick landing on a multiple of 96 (four 24-tick beats) while running.
      const bool downbeat = clock_.running() && clock_.ticks() % 96 == 0;
      if (clock_.tick(frame)) takeTick(frame, downbeat);
      return;
    }
    case 0xFA: clock_.start(frame); return;
    case 0xFB: clock_.cont(frame); return;
    case 0xFC:
      clock_.stop(frame);
      takeClockStopped();
      return;
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
          if (e->type() == EngineType::Drum)
            for (uint16_t pads = static_cast<DrumEngine*>(e)->padsForNote(m.data1 & 127); pads;) {
              const int pad = __builtin_ctz(pads);
              pads &= static_cast<uint16_t>(pads - 1);
              status_.padHits[ti][static_cast<std::size_t>(pad)].fetch_add(1, std::memory_order_relaxed);
            }
        } else {
          e->noteOff(m.data1);
        }
        break;
      case 0x80: e->noteOff(m.data1); break;
      case 0xB0: e->controlChange(m.data1, m.data2); break;
      case 0xE0: e->pitchBend(((m.data2 << 7) | m.data1) - 8192); break;
      case 0xC0:
        if (programs_ && m.data1 < programs_->count && e->type() == EngineType::Fm) {
          static_cast<FmEngine*>(e)->loadVoice(programs_->voices[m.data1]);
          Notification n{Notification::Type::ProgramChanged};
          n.track = static_cast<uint8_t>(t);
          n.program = m.data1;
          notify(n);
        }
        break;
      default: break;
    }
  }
}

const ParamInfo* Module::info(ParamRef ref) const noexcept {
  if (ref.scope >= ParamStore::kScopes) return nullptr;
  if (ref.isInsert()) {
    if (ref.slotIndex() == 0) return &insertOnInfo();
    const Effect* e = fx_[ref.scope][static_cast<std::size_t>(ref.slot())];
    if (!e) return nullptr;
    const auto table = effectParams(e->type());
    const std::size_t i = static_cast<std::size_t>(ref.slotIndex() - 1);
    return i < table.size() ? &table[i] : nullptr;
  }
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
  else if (ref.isInsert() && ref.slotIndex() > 0) fx_[ref.scope][static_cast<std::size_t>(ref.slot())]->setParam(ref.slotIndex() - 1, value);
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
      for (PreviewVoice& p : preview_) {
        releaseSample(p.sample);
        p = {};
      }
      // Silence means the tails too.
      for (auto& scope : fx_)
        for (Effect* e : scope)
          if (e) e->reset();
      limiter_.reset();
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
          const ParamRef ref{static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + i)};
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
    case Command::Type::SetSample:
      if (Engine* e = engines_[track]; e && (e->type() == EngineType::Drum || e->type() == EngineType::Sampler))
        e->setSample(c.slot, c.sample);
      else
        releaseSample(c.sample);  // the track changed engine meanwhile: never used
      break;
    case Command::Type::Preview: {
      // The one playing fades out; one already fading is cut (it's nearly silent).
      for (PreviewVoice& p : preview_) {
        if (!p.sample) continue;
        if (p.fading) {
          releaseSample(p.sample);
          p = {};
        } else {
          p.fading = true;
        }
      }
      if (c.sample) {
        PreviewVoice& slot = preview_[0].sample ? preview_[1] : preview_[0];
        if (slot.sample) releaseSample(slot.sample);
        slot = {c.sample, 0, c.value, 1, false};
      }
      break;
    }
    case Command::Type::SetInsert: {
      const auto scope = static_cast<std::size_t>(c.scope);
      const bool fixed = c.scope >= kDelayScope && c.slot == 0;  // the returns' own effects stay
      if (scope >= kScopeCount || c.slot < 0 || c.slot >= kInsertSlots || fixed) {
        if (c.effect) {
          Notification n{Notification::Type::EffectRetired};
          n.effect = c.effect;
          notify(n);
        }
        break;
      }
      const auto slot = static_cast<std::size_t>(c.slot);
      Effect* old = fx_[scope][slot];
      fx_[scope][slot] = c.effect;
      // A new insert starts at its defaults, switched in, and fades in over a block.
      fxMix_[scope][slot] = 0;
      status_.reduction[scope][slot].store(0, std::memory_order_relaxed);
      const ParamRef on = insertOnRef(c.scope, c.slot);
      values_[scope][on.index] = 1;
      store_.publish(on, 1);
      if (c.effect) {
        const auto table = effectParams(c.effect->type());
        for (std::size_t i = 0; i < table.size(); ++i) {
          const ParamRef ref = insertParamRef(c.scope, c.slot, static_cast<int>(i));
          values_[scope][ref.index] = table[i].def;
          store_.publish(ref, table[i].def);
        }
      }
      if (old) {
        Notification n{Notification::Type::EffectRetired};
        n.effect = old;
        notify(n);
      }
      break;
    }
    case Command::Type::Record:
      if (c.take) startTake(c.take);
      break;
    case Command::Type::StopRecord:
      if (take_) {
        const Take::State st = take_->state.load(std::memory_order_relaxed);
        if (st == Take::State::Armed) {
          endTake(Take::State::Cancelled);
        } else if (take_->synced) {
          // On the next downbeat: the bar under way is the last.
          const int bars = takeTicks_ / kTicksPerBar + 1;
          if (bars < take_->targetBars.load(std::memory_order_relaxed)) take_->targetBars.store(bars, std::memory_order_relaxed);
        } else if (takeEnd_ < 0 || takeEnd_ > frames_) {
          take_->cutShort = take_->bars > 0;  // short of the bars it was asked for
          takeEnd_ = frames_;                 // the capture ends it
        }
      }
      break;
    case Command::Type::CancelRecord:
      endTake(Take::State::Cancelled);
      break;
    case Command::Type::SetPlaitsBank: {
      PlaitsBank* old = c.plaitsBank;  // no Plaits on the track: the bank goes straight back
      if (Engine* e = engines_[track]; e && e->type() == EngineType::Plaits) old = static_cast<PlaitsEngine*>(e)->setBank(c.plaitsBank);
      if (old) {
        Notification n{Notification::Type::BankRetired};
        n.plaitsBank = old;
        notify(n);
      }
      break;
    }
    case Command::Type::SetPrograms: {
      ProgramTable* old = programs_;
      programs_ = c.programs;
      if (old) {
        Notification n{Notification::Type::ProgramsRetired};
        n.programs = old;
        notify(n);
      }
      break;
    }
  }
}

}  // namespace ion
