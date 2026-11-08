#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace ion {

// What a take records (HANDOFF §7.6). Audio input slots in here later.
enum class RecSource : uint8_t { Master, Tracks };
// Where a track is tapped: after its EQ, inserts, fader and pan (mute and
// solo ignored, so a muted track still records), or the engine's dry output.
enum class RecTap : uint8_t { Post, Pre };

// One recording, made on the control thread and filled on the audio thread
// (DECISIONS D-51).
//
// The control thread allocates the buffers (zeroed, so no page faults on the
// audio thread), sends the take with Command::Record and keeps owning it. The
// audio thread writes samples, then publishes `frames` with release order, so
// the UI may read anything below `frames` while it records (the live
// waveform). When the take ends the audio thread drops its pointer and sends
// Notification::TakeDone; only then may the control thread change or free it.
struct Take {
  // ---- set before it's sent, never changed after ----
  RecSource source = RecSource::Master;
  uint8_t tracks = 0;  // RecSource::Tracks: bit t = track t
  RecTap tap = RecTap::Post;
  bool synced = true;  // start on the next downbeat of MIDI clock; else at once
  int bars = 4;        // 1-16; 0 = until stopped
  int64_t capacity = 0;
  std::vector<float> left, right;  // capacity frames each
  std::vector<int64_t> ticks;      // synced: frame of each clock tick from the start (tick 0 is 0)

  enum class State : uint8_t { Armed, Recording, Done, Cancelled };
  std::atomic<State> state{State::Armed};
  std::atomic<int64_t> frames{0};  // captured so far
  std::atomic<int> tickCount{0};   // ticks stored in `ticks`
  std::atomic<int> targetBars{0};  // bars it will stop after (stopping early lowers it)

  // ---- written by the audio thread; read once `state` is Done ----
  int64_t startFrame = 0;  // the module frame the take starts on
  int startBar = 0;        // synced: the clock's bar it started on, 0-based
  double bpm = 0;          // the tempo when it started (clock, or 120 without one)
  bool cutShort = false;   // the clock stopped or went away mid-take, or it ran out of room
};

}  // namespace ion
