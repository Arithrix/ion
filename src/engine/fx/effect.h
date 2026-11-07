#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/params.h"

namespace ion {

// Insert and bus effects (HANDOFF §7). Like engines, effects are pure DSP:
// built and freed off the audio thread, everything below runs on it.
enum class EffectType : uint8_t { None, Compressor, Chorus, Delay, Reverb };
inline constexpr int kEffectTypes = 5;

const char* effectKey(EffectType type);    // "compressor", in files
const char* effectLabel(EffectType type);  // "COMP", in the UI
EffectType effectFromKey(const char* key);
std::span<const ParamInfo> effectParams(EffectType type);

class Effect {
 public:
  virtual ~Effect() = default;
  virtual EffectType type() const = 0;
  // index into effectParams(type()); value already clamped.
  virtual void setParam(int index, float value) = 0;
  // In place, 1…kBlockSize frames.
  virtual void process(float* left, float* right, std::size_t frames) = 0;
  // Clears tails and envelopes (panic).
  virtual void reset() = 0;

  // On a send bus the effect is the whole return: wet only, its Mix ignored.
  virtual void setWetOnly(bool) {}
  // Tempo in BPM, every block, for tempo-synced times.
  virtual void setTempo(double) {}
  // Gain reduction in dB (≥ 0) for the meters; 0 for effects that don't compress.
  virtual float reduction() const { return 0; }
};

// Allocates everything up front (delay lines, reverb tank): not on the audio thread.
std::unique_ptr<Effect> createEffect(EffectType type);

}  // namespace ion
