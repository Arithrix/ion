#pragma once

// What a DX7 voice means, beyond its bytes: which operator modulates which
// (per algorithm), and the front-panel macros (HANDOFF §6.3).
//
// Operators are numbered 1-6 as on the DX7. In voice data the first block of
// 21 bytes is operator 6 and the last is operator 1; opOffset() hides that.

#include <array>
#include <cstdint>
#include <string>

#include "library/syx.h"

namespace ion::dx7 {

inline constexpr int kAlgorithms = 32;

// Byte offsets in an unpacked (155-byte) voice.
inline constexpr std::size_t kAlgorithmByte = 134;
inline constexpr std::size_t kFeedbackByte = 135;
inline constexpr std::size_t kTransposeByte = 144;
constexpr std::size_t opOffset(int op) { return static_cast<std::size_t>(6 - op) * 21; }  // op 1-6
// Within an operator block.
inline constexpr std::size_t kOpRate1 = 0, kOpRate4 = 3, kOpVelocity = 15, kOpLevel = 16, kOpDetune = 20;

struct Topology {
  std::array<uint8_t, 7> modulators{};  // [op] = bitmask of ops modulating op (bit n = op n)
  uint8_t carriers = 0;                 // bitmask of ops heard directly
  uint8_t feedbackFrom = 0;             // the feedback loop: from this op's output…
  uint8_t feedbackTo = 0;               // …into this op's input (the same op when it's a self-loop)
  bool carrier(int op) const { return carriers & (1u << op); }
};

// algorithm 0-31, as stored in voice byte 134. Taken from Synth_Dexed's own
// table, so it always matches what the engine plays.
const Topology& topology(int algorithm);

// "three 2-op stacks", "six carriers": a short, plain description.
std::string describe(int algorithm);

// ---- macros ----

// Non-destructive offsets in DX7 parameter space, applied over a stored voice.
// Every result is clamped into the valid DX7 range, so a voice with macros
// applied is always a valid voice (and "save as" can bake it).
struct Macros {
  int bright = 0;    // ±50: output level of modulators (operators that aren't carriers)
  int feedback = 0;  // ±7: the voice's feedback
  int detune = 0;    // ±7: + spreads operator detune apart, − pulls it to centre
  int velocity = 0;  // ±7: key velocity sensitivity of carriers
  bool any() const { return bright || feedback || detune || velocity; }
  bool operator==(const Macros&) const = default;
};

// Attack and Release were macros until 0.5.0; the uniform amp envelope
// replaced them (DECISIONS D-50).
inline constexpr int kMacroCount = 4;
inline constexpr std::array<const char*, kMacroCount> kMacroNames = {"Bright", "Feedback", "Detune", "Velocity"};

// Real-time safe (no allocation). With all macros at 0 the voice is returned unchanged.
syx::Voice applyMacros(const syx::Voice& voice, const Macros& m);

}  // namespace ion::dx7
