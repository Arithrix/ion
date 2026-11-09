#pragma once

// One track's modulation sources (DECISIONS D-58): two LFOs, two envelopes,
// and what the track's notes and controllers say. Audio thread only; no
// allocation. The Module reads the sources once a block and applies the
// track's routes.

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/params.h"
#include "dsp/ahdsr.h"

namespace ion {

class TrackModulation {
 public:
  // `mod` is the track's modulation values, kModParamCount of them from kModBase.
  // Moves the LFOs and envelopes on by `frames`. `bpm` is the tempo synced LFOs follow.
  void advance(const float* mod, double bpm, std::size_t frames) noexcept;
  // A note on the track: fires the envelopes (as their trigger says), restarts
  // LFOs set to restart on notes, and sets velocity, key and random.
  void noteOn(const float* mod, uint8_t note, uint8_t velocity) noexcept;
  void noteOff(uint8_t note) noexcept;
  void modWheel(uint8_t value) noexcept { wheel_ = static_cast<float>(value) / 127.0f; }
  void aftertouch(uint8_t value) noexcept { touch_ = static_cast<float>(value) / 127.0f; }
  // A downbeat of the OXI's clock, `bar` bars since its start: LFOs set to
  // restart on bars do; synced LFOs line up with it.
  void downbeat(const float* mod, int64_t bar) noexcept;
  void reset() noexcept;

  // A source's value now: −1…1 for the bipolar ones (LFOs, random), 0…1 for the rest.
  float source(int s) const noexcept;
  float lfoPhase(int lfo) const noexcept { return static_cast<float>(lfo_[static_cast<std::size_t>(lfo)].phase); }

 private:
  struct Lfo {
    double phase = 0;     // 0…1
    float value = 0;
    float from = 0, to = 0;  // S&H and smooth: the last value and the next
    uint32_t rng = 1;
  };
  static float shape(int s, double phase, const Lfo& l) noexcept;
  void newStep(Lfo& l) noexcept;
  float random() noexcept;
  dsp::Ahdsr::Coeffs coeffs(const float* mod, int env) const noexcept;

  std::array<Lfo, kLfos> lfo_{};
  std::array<dsp::Ahdsr, kModEnvs> env_{};
  std::array<uint32_t, 4> held_{};  // 128 bits: notes down on the track
  int heldCount_ = 0;
  float velocity_ = 0, key_ = 0, wheel_ = 0, touch_ = 0, random_ = 0;
  uint32_t rng_ = 0x9E3779B9u;
};

}  // namespace ion
