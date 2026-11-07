#pragma once

#include <cstddef>
#include <cstdint>

namespace ion {

inline constexpr int kSampleRate = 48000;

// Every engine renders in blocks of exactly this many frames. 64 matches the
// DX7 engine's native block; Plaits (12) will loop internally.
inline constexpr std::size_t kBlockSize = 64;

// The interface every instrument engine implements (HANDOFF §4). All methods
// are called on the audio thread and must not allocate, lock or block.
// setParam and serialize/deserialize arrive in Phase 1 with project files.
class Engine {
 public:
  virtual ~Engine() = default;

  virtual void noteOn(uint8_t note, uint8_t velocity) = 0;
  virtual void noteOff(uint8_t note) = 0;
  virtual void controlChange(uint8_t controller, uint8_t value) = 0;
  // -8192 … 8191
  virtual void pitchBend(int value) = 0;
  // Silences everything immediately (MIDI panic).
  virtual void allNotesOff() = 0;

  // Adds kBlockSize frames into left and right.
  virtual void render(float* left, float* right) = 0;

  // Voices currently producing sound.
  virtual int voiceCount() const = 0;
};

}  // namespace ion
