#pragma once

#include <cstdint>
#include <span>

namespace ion {

// Which voice a new note gets (HANDOFF §5, DECISIONS D-26): a free one; else
// the oldest one already released (it's fading anyway); else the oldest held.
// Shared by the Drum and Sampler engines. V needs `bool active() const`,
// `bool released() const` and `uint32_t age` (a counter that grows with each
// note-on, so a smaller age is older).
template <class V>
int pickVoice(std::span<const V> voices) {
  int released = -1, held = -1;
  for (int i = 0; i < static_cast<int>(voices.size()); ++i) {
    const V& v = voices[static_cast<std::size_t>(i)];
    if (!v.active()) return i;
    int& best = v.released() ? released : held;
    if (best < 0 || static_cast<int32_t>(v.age - voices[static_cast<std::size_t>(best)].age) < 0) best = i;
  }
  return released >= 0 ? released : held;
}

}  // namespace ion
