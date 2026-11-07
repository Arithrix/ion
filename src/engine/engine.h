#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/params.h"

namespace ion {

inline constexpr int kSampleRate = 48000;

// The module renders in blocks of this many frames; MIDI can split a block, so
// engines get render() calls of 1…kBlockSize frames. 64 is Synth_Dexed's native
// block (FM note-ons land on its 64-frame grid; other engines are sample-exact).
inline constexpr std::size_t kBlockSize = 64;

enum class EngineType : uint8_t { None, Fm, Drum, Sampler, Plaits };

// Name in files ("fm") and in the UI ("FM").
const char* engineKey(EngineType type);
const char* engineLabel(EngineType type);
EngineType engineFromKey(const char* key);
// The engine's parameter table: indices are ParamRef::index - kEngineParamBase.
std::span<const ParamInfo> engineParams(EngineType type);

// The interface every instrument engine implements (HANDOFF §4).
//
// Engines are pure DSP. Everything here runs on the audio thread and must not
// allocate, lock or block. Construction and destruction happen on other
// threads; the module hands engines over through lock-free queues.
//
// Saving and loading go through the Session's state, not the engine object:
// the audio thread owns the engine, so no other thread may read it (DECISIONS D-18).
class Engine {
 public:
  virtual ~Engine() = default;
  virtual EngineType type() const = 0;

  virtual void noteOn(uint8_t note, uint8_t velocity) = 0;
  virtual void noteOff(uint8_t note) = 0;
  virtual void controlChange(uint8_t controller, uint8_t value) = 0;
  virtual void pitchBend(int value) = 0;  // -8192 … 8191
  virtual void allNotesOff() = 0;         // MIDI panic: silence now

  // index into engineParams(type()); value already clamped to its range.
  virtual void setParam(int index, float value) = 0;

  // Adds `frames` (1…kBlockSize) frames into left and right.
  virtual void render(float* left, float* right, std::size_t frames) = 0;

  virtual int voiceCount() const = 0;
};

}  // namespace ion
