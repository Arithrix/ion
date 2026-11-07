#pragma once

#include <array>
#include <cstdint>

namespace ion {

// Follows external MIDI clock (24 ticks per quarter note) plus start, stop,
// continue and song position. Works in the audio frame domain: every message
// carries the frame it lands on, so tempo comes from sample-accurate spacing.
//
// Tempo is the slope of a least-squares line through the last 48 tick times
// (two beats). That averages away USB jitter (±1 ms on each tick moves the
// estimate by a few hundredths of a BPM), so delay times don't wobble, and a
// real tempo change has fully replaced the window two beats later.
class ClockFollower {
 public:
  explicit ClockFollower(double sampleRate) : sr_(sampleRate) {}

  void tick(int64_t frame);
  void start(int64_t frame);     // FA: position back to 0, running
  void cont(int64_t frame);      // FB: running from the current position
  void stop(int64_t frame);      // FC
  void songPosition(int beats);  // F2: position in 16ths (MIDI beats)

  // Call once per block with the block's first frame, to notice lost clock.
  void update(int64_t frame);

  bool present() const { return present_; }  // ticks arrived recently
  bool running() const { return running_; }  // transport started and not stopped
  double bpm() const { return bpm_; }        // 0 until two ticks have arrived
  int64_t ticks() const { return ticks_; }   // since start, 24 per quarter
  int bar() const { return static_cast<int>(ticks_ / 96); }  // 0-based, 4/4
  int beat() const { return static_cast<int>((ticks_ / 24) % 4); }
  int64_t lastTickFrame() const { return lastTick_; }

 private:
  double sr_;
  static constexpr int kWindow = 48;
  std::array<int64_t, kWindow> window_{};  // recent tick frames, a ring
  int count_ = 0;
  int head_ = 0;
  int64_t lastTick_ = -1;
  double bpm_ = 0;
  int64_t ticks_ = 0;
  bool running_ = false;
  bool present_ = false;
};

}  // namespace ion
