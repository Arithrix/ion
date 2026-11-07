#pragma once

#include <array>

#include "engine/sample.h"

namespace ion {

// Samples an engine has taken off its pads (or its zone) but may still be
// playing. Each is handed back (pop) once no voice uses it, so a tail always
// finishes with its own sample (DECISIONS D-36). Audio thread only; no
// allocation. N is how many can wait at once.
template <int N>
class SampleRetirement {
 public:
  // `stop(s)` silences the voices playing s; used only when all N places are
  // taken (N loads while long tails still ring), to make room.
  template <class Stop, class InUse>
  void retire(const Sample* s, Stop stop, InUse inUse) {
    if (!s) return;
    if (retiring_ == N) {
      stop(retired_[0]);
      update(inUse);
    }
    if (retiring_ < N) retired_[static_cast<std::size_t>(retiring_++)] = s;
    update(inUse);
  }

  // Moves samples no voice uses (`inUse(s)` false) to the hand-back list.
  template <class InUse>
  void update(InUse inUse) {
    int keep = 0;
    for (int i = 0; i < retiring_; ++i) {
      const Sample* s = retired_[static_cast<std::size_t>(i)];
      if (!inUse(s) && released_ < N) released_list_[static_cast<std::size_t>(released_++)] = s;
      else retired_[static_cast<std::size_t>(keep++)] = s;
    }
    retiring_ = keep;
  }

  bool pending() const { return retiring_ > 0; }
  bool pop(const Sample*& s) {
    if (released_ == 0) return false;
    s = released_list_[static_cast<std::size_t>(--released_)];
    return true;
  }
  // Every sample still held here, waiting or ready to hand back.
  template <class F>
  void forEach(F&& f) const {
    for (int i = 0; i < retiring_; ++i) f(retired_[static_cast<std::size_t>(i)]);
    for (int i = 0; i < released_; ++i) f(released_list_[static_cast<std::size_t>(i)]);
  }

 private:
  std::array<const Sample*, N> retired_{}, released_list_{};
  int retiring_ = 0, released_ = 0;
};

}  // namespace ion
