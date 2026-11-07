#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "core/spsc_queue.h"
#include "engine/engine.h"
#include "library/syx.h"

namespace ion {

class FmEngine;

inline constexpr int kTrackCount = 8;

// A raw MIDI channel or system message.
struct MidiMessage {
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
};

// UI → audio thread.
struct Command {
  enum class Type : uint8_t { NoteOn, NoteOff, Panic, LoadVoice };
  Type type = Type::Panic;
  uint8_t track = 0;
  uint8_t note = 0;
  uint8_t velocity = 0;
  syx::Voice voice{};
};

// Written by the audio thread, read by anyone.
struct ModuleStatus {
  std::atomic<float> dspLoad{0};      // smoothed fraction of the callback period
  std::atomic<float> dspLoadPeak{0};  // worst single callback since start
  std::atomic<uint32_t> callbackFrames{0};
  std::atomic<uint64_t> framesRendered{0};
  std::atomic<int> voices{0};
  std::atomic<uint32_t> midiReceived{0};
  std::atomic<uint32_t> lastMidi{0};  // status | data1 << 8 | data2 << 16
  std::atomic<uint32_t> droppedMidi{0};
  std::atomic<uint32_t> droppedCommands{0};
  std::atomic<float> peakLeft{0};  // decaying peak, linear
  std::atomic<float> peakRight{0};
  std::atomic<uint32_t> clippedBlocks{0};
  std::array<std::atomic<float>, kTrackCount> trackPeak{};
  std::array<std::atomic<uint32_t>, kTrackCount> trackNotes{};  // note-on counter, for activity lights
};

// The whole sound module: eight tracks, MIDI dispatch, output. Phase 0 has no
// mixer yet; tracks sum straight to a safety clipper.
class Module {
 public:
  Module();
  ~Module();

  // ---- setup (before audio starts) ----
  void setFmEngine(int track, std::unique_ptr<FmEngine> engine);
  bool hasEngine(int track) const { return engines_[track] != nullptr; }
  int channel(int track) const { return channels_[track]; }  // 0-based MIDI channel

  // ---- producers: each queue has exactly one producer thread ----
  bool pushMidi(const MidiMessage& m);   // MIDI thread only
  bool pushCommand(const Command& c);    // UI thread only

  // ---- audio thread ----
  // Fills `frames` interleaved stereo frames. Any frame count works; rendering
  // happens in kBlockSize blocks, so when frames is a multiple of kBlockSize
  // no latency is added.
  void process(float* interleaved, std::size_t frames) noexcept;

  const ModuleStatus& status() const { return status_; }

 private:
  void renderBlock() noexcept;
  void handleMidi(const MidiMessage& m) noexcept;
  void handleCommand(const Command& c) noexcept;

  std::array<std::unique_ptr<Engine>, kTrackCount> engines_{};
  std::array<FmEngine*, kTrackCount> fm_{};
  std::array<int, kTrackCount> channels_{};

  SpscQueue<MidiMessage, 1024> midiQueue_;
  SpscQueue<Command, 256> commandQueue_;

  float left_[kBlockSize]{};
  float right_[kBlockSize]{};
  float trackLeft_[kBlockSize]{};
  float trackRight_[kBlockSize]{};
  std::size_t blockPos_ = kBlockSize;  // kBlockSize = "need a new block"
  uint64_t frames_ = 0;
  float peakLeft_ = 0, peakRight_ = 0;
  std::array<float, kTrackCount> trackPeak_{};
  float load_ = 0;

  ModuleStatus status_;
};

}  // namespace ion
