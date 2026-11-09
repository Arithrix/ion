#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "core/params.h"

namespace ion {

// Current values of every parameter, readable from any thread.
//
// The audio thread is the only writer: UI edits arrive as commands and CC moves
// arrive as MIDI, and both are applied there, then published here. The UI reads
// this to draw values, so CC moves show up on screen; Session reads it to save.
class ParamStore {
 public:
  static constexpr int kPerScope = kModBase + kModRange;  // 512: mixer, inserts, engine, modulation
  static constexpr int kScopes = kScopeCount;                             // tracks, master, returns

  ParamStore() {
    for (int s = 0; s < kScopes; ++s)
      for (int i = 0; i < kMixerParamCount; ++i)
        values_[index({static_cast<uint8_t>(s), static_cast<uint8_t>(i)})].store(
            mixerParams()[static_cast<std::size_t>(i)].def, std::memory_order_relaxed);
  }

  float get(ParamRef r) const { return values_[index(r)].load(std::memory_order_relaxed); }

  // Audio thread only.
  void publish(ParamRef r, float v) {
    values_[index(r)].store(v, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
  }

  // Bumped on every change; Session compares it to know there's something to autosave.
  uint32_t version() const { return version_.load(std::memory_order_acquire); }

  static constexpr int index(ParamRef r) { return r.scope * kPerScope + r.index; }

 private:
  std::array<std::atomic<float>, kScopes * kPerScope> values_{};
  std::atomic<uint32_t> version_{0};
};

}  // namespace ion
