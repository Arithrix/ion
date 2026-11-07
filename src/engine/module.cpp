#include "engine/module.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/denormals.h"
#include "engine/fm/fm_engine.h"

namespace ion {
namespace {

// −0.3 dBFS. A hard ceiling so nothing reaches the speakers above full scale.
// Phase 4 replaces this with the master limiter.
constexpr float kCeiling = 0.9660509f;
// Meter decay per 64-frame block: about 20 dB per second.
constexpr float kPeakDecay = 0.99694f;

}  // namespace

Module::Module() {
  for (int t = 0; t < kTrackCount; ++t) channels_[t] = t;
}

Module::~Module() = default;

void Module::setFmEngine(int track, std::unique_ptr<FmEngine> engine) {
  fm_[track] = engine.get();
  engines_[track] = std::move(engine);
}

bool Module::pushMidi(const MidiMessage& m) {
  if (midiQueue_.push(m)) return true;
  status_.droppedMidi.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool Module::pushCommand(const Command& c) {
  if (commandQueue_.push(c)) return true;
  status_.droppedCommands.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void Module::process(float* out, std::size_t frames) noexcept {
  disableDenormals();
  const auto start = std::chrono::steady_clock::now();

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
  while (auto m = midiQueue_.pop()) handleMidi(*m);

  FmEngine::setClock(frames_);

  std::memset(left_, 0, sizeof left_);
  std::memset(right_, 0, sizeof right_);
  int voices = 0;
  for (int t = 0; t < kTrackCount; ++t) {
    Engine* e = engines_[t].get();
    if (!e) continue;
    std::memset(trackLeft_, 0, sizeof trackLeft_);
    std::memset(trackRight_, 0, sizeof trackRight_);
    e->render(trackLeft_, trackRight_);
    float peak = 0;
    for (std::size_t i = 0; i < kBlockSize; ++i) {
      left_[i] += trackLeft_[i];
      right_[i] += trackRight_[i];
      peak = std::max(peak, std::max(std::fabs(trackLeft_[i]), std::fabs(trackRight_[i])));
    }
    trackPeak_[t] = std::max(peak, trackPeak_[t] * kPeakDecay);
    status_.trackPeak[t].store(trackPeak_[t], std::memory_order_relaxed);
    voices += e->voiceCount();
  }

  float pl = 0, pr = 0;
  bool clipped = false;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    float& l = left_[i];
    float& r = right_[i];
    if (!std::isfinite(l)) l = 0;  // never let a NaN reach the speakers
    if (!std::isfinite(r)) r = 0;
    pl = std::max(pl, std::fabs(l));
    pr = std::max(pr, std::fabs(r));
    if (std::fabs(l) > kCeiling || std::fabs(r) > kCeiling) clipped = true;
    l = std::clamp(l, -kCeiling, kCeiling);
    r = std::clamp(r, -kCeiling, kCeiling);
  }
  peakLeft_ = std::max(pl, peakLeft_ * kPeakDecay);
  peakRight_ = std::max(pr, peakRight_ * kPeakDecay);
  status_.peakLeft.store(peakLeft_, std::memory_order_relaxed);
  status_.peakRight.store(peakRight_, std::memory_order_relaxed);
  if (clipped) status_.clippedBlocks.fetch_add(1, std::memory_order_relaxed);
  status_.voices.store(voices, std::memory_order_relaxed);

  frames_ += kBlockSize;
  status_.framesRendered.store(frames_, std::memory_order_relaxed);
}

void Module::handleMidi(const MidiMessage& m) noexcept {
  status_.midiReceived.fetch_add(1, std::memory_order_relaxed);
  status_.lastMidi.store(m.status | (m.data1 << 8) | (m.data2 << 16), std::memory_order_relaxed);

  if (m.status >= 0xF0) return;  // clock, start/stop: Phase 1
  const int type = m.status & 0xF0;
  const int ch = m.status & 0x0F;
  for (int t = 0; t < kTrackCount; ++t) {
    if (channels_[t] != ch || !engines_[t]) continue;
    Engine& e = *engines_[t];
    switch (type) {
      case 0x90:
        if (m.data2 > 0) {
          e.noteOn(m.data1, m.data2);
          status_.trackNotes[t].fetch_add(1, std::memory_order_relaxed);
        } else {
          e.noteOff(m.data1);
        }
        break;
      case 0x80: e.noteOff(m.data1); break;
      case 0xB0: e.controlChange(m.data1, m.data2); break;
      case 0xE0: e.pitchBend(((m.data2 << 7) | m.data1) - 8192); break;
      default: break;
    }
  }
}

void Module::handleCommand(const Command& c) noexcept {
  switch (c.type) {
    case Command::Type::NoteOn:
      if (engines_[c.track]) {
        engines_[c.track]->noteOn(c.note, c.velocity);
        status_.trackNotes[c.track].fetch_add(1, std::memory_order_relaxed);
      }
      break;
    case Command::Type::NoteOff:
      if (engines_[c.track]) engines_[c.track]->noteOff(c.note);
      break;
    case Command::Type::Panic:
      for (auto& e : engines_)
        if (e) e->allNotesOff();
      break;
    case Command::Type::LoadVoice:
      if (fm_[c.track]) fm_[c.track]->loadVoice(c.voice);
      break;
  }
}

}  // namespace ion
