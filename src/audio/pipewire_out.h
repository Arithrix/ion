#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct pw_thread_loop;
struct pw_stream;

namespace ion {

class Module;

// Plays the module through PipeWire as an ordinary playback stream (DECISIONS D-02):
// it follows the default output device, mixes with other apps, opens no input,
// and requests (never forces) a small graph quantum.
class PipeWireOutput {
 public:
  struct Options {
    uint32_t latencyFrames = 128;  // requested quantum at 48 kHz
  };

  PipeWireOutput(Module& module, Options options);
  ~PipeWireOutput();
  PipeWireOutput(const PipeWireOutput&) = delete;
  PipeWireOutput& operator=(const PipeWireOutput&) = delete;

  // Connects and waits until the stream is running. On failure returns false
  // and sets `error`.
  bool start(std::string& error);
  void stop();

  // Callbacks where the graph skipped ahead of us (an xrun, as seen from our side).
  uint32_t dropouts() const { return dropouts_.load(std::memory_order_relaxed); }
  // Whether PipeWire's audio thread is running with real-time priority.
  bool realtime() const { return realtime_.load(std::memory_order_relaxed); }
  uint32_t requestedFrames() const { return options_.latencyFrames; }

  // PipeWire callbacks (public only so the C callback table can reach them).
  static void onProcess(void* self);
  static void onStateChanged(void* self, int oldState, int state, const char* error);

 private:

  Module& module_;
  Options options_;
  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  std::atomic<int> state_{0};
  std::atomic<uint32_t> dropouts_{0};
  std::atomic<bool> realtime_{false};
  bool checkedRealtime_ = false;
  uint64_t lastTicks_ = 0;
  uint32_t lastFrames_ = 0;
  std::string lastError_;
};

}  // namespace ion
