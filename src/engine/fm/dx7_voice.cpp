#include "engine/fm/dx7_voice.h"

#include <algorithm>
#include <cmath>

#include <fm_core.h>

namespace ion::dx7 {
namespace {

// FmCore keeps its algorithm table protected; this only reads it.
struct AlgorithmTable : FmCore {
  using FmCore::algorithms;
};

// Synth_Dexed describes each algorithm as bus operations, run from operator 6
// down to 1: each operator reads bus 1 or 2 (its modulators), then either
// replaces a bus with itself, adds itself to one, or adds itself to the output.
Topology fromBusFlags(const FmAlgorithm& alg) {
  Topology t;
  uint8_t bus[3] = {0, 0, 0};  // bitmask of the ops currently summed on bus 1, 2
  for (int i = 0; i < 6; ++i) {
    const int op = 6 - i;
    const auto flags = static_cast<uint32_t>(alg.ops[i]);
    if (flags & IN_BUS_ONE) t.modulators[static_cast<std::size_t>(op)] |= bus[1];
    if (flags & IN_BUS_TWO) t.modulators[static_cast<std::size_t>(op)] |= bus[2];
    const uint32_t out = flags & 7;
    const auto self = static_cast<uint8_t>(1u << op);
    if (out == OUT_BUS_ADD) t.carriers |= self;
    else if (out & OUT_BUS_ADD) bus[out & 3] |= self;
    else bus[out & 3] = self;
    if (flags & FB_IN) t.feedbackTo = static_cast<uint8_t>(op);
    if (flags & FB_OUT) t.feedbackFrom = static_cast<uint8_t>(op);
  }
  return t;
}

const std::array<Topology, kAlgorithms> kTopologies = [] {
  std::array<Topology, kAlgorithms> all{};
  for (int a = 0; a < kAlgorithms; ++a) all[static_cast<std::size_t>(a)] = fromBusFlags(AlgorithmTable::algorithms[a]);
  return all;
}();

// One line per algorithm, written against the topology above (see tests).
const std::array<const char*, kAlgorithms> kDescriptions = {
    "a 4-op stack and a 2-op stack",                // 1
    "a 4-op stack and a 2-op stack",                // 2
    "two 3-op stacks",                              // 3
    "two 3-op stacks, feedback loop",               // 4
    "three 2-op stacks",                            // 5
    "three 2-op stacks, feedback loop",             // 6
    "two modulators into one, plus a 2-op stack",   // 7
    "two modulators into one, plus a 2-op stack",   // 8
    "two modulators into one, plus a 2-op stack",   // 9
    "a 3-op stack and a 2-into-1 pair",             // 10
    "a 3-op stack and a 2-into-1 pair",             // 11
    "three modulators into one, plus a 2-op stack", // 12
    "three modulators into one, plus a 2-op stack", // 13
    "two modulators on a stack, plus a 2-op stack", // 14
    "two modulators on a stack, plus a 2-op stack", // 15
    "one carrier, three modulator branches",        // 16
    "one carrier, three modulator branches",        // 17
    "one carrier, three modulator branches",        // 18
    "a 3-op stack and one modulator on two",        // 19
    "one modulator on two, two into one",           // 20
    "two modulators, each on two carriers",         // 21
    "a 2-op stack and one modulator on three",      // 22
    "one modulator on two, a 2-op stack, a carrier",// 23
    "one modulator on three, two carriers",         // 24
    "one modulator on two, three carriers",         // 25
    "a 2-op stack, two into one, a carrier",        // 26
    "a 2-op stack, two into one, a carrier",        // 27
    "a 3-op stack, a 2-op stack, a carrier",        // 28
    "two 2-op stacks and two carriers",             // 29
    "a 3-op stack and three carriers",              // 30
    "a 2-op stack and four carriers",               // 31
    "six carriers: additive, organ-like",           // 32
};

uint8_t clampByte(int v, int hi) { return static_cast<uint8_t>(std::clamp(v, 0, hi)); }

}  // namespace

const Topology& topology(int algorithm) { return kTopologies[static_cast<std::size_t>(std::clamp(algorithm, 0, 31))]; }

std::string describe(int algorithm) { return kDescriptions[static_cast<std::size_t>(std::clamp(algorithm, 0, 31))]; }

syx::Voice applyMacros(const syx::Voice& voice, const Macros& m) {
  if (!m.any()) return voice;
  syx::Voice v = voice;
  const Topology& t = topology(v[kAlgorithmByte]);
  // Detune spread: neighbouring operators go opposite ways, outer ones further.
  constexpr float kSpread[7] = {0, 1.0f, -1.0f, 0.67f, -0.67f, 0.33f, -0.33f};
  for (int op = 1; op <= 6; ++op) {
    uint8_t* o = v.data() + opOffset(op);
    const bool carrier = t.carrier(op);
    // Silent modulators stay silent: bringing in an unused operator would
    // change the patch rather than brighten it.
    if (!carrier && m.bright && o[kOpLevel] > 0) o[kOpLevel] = clampByte(o[kOpLevel] + m.bright, 99);
    o[kOpRate1] = clampByte(o[kOpRate1] - m.attack, 99);
    o[kOpRate4] = clampByte(o[kOpRate4] - m.release, 99);
    if (m.detune > 0) {
      o[kOpDetune] = clampByte(o[kOpDetune] + static_cast<int>(std::lround(static_cast<float>(m.detune) * kSpread[op])), 14);
    } else if (m.detune < 0) {
      const float keep = 1.0f + static_cast<float>(m.detune) / 7.0f;  // −7 → all at centre
      o[kOpDetune] = clampByte(7 + static_cast<int>(std::lround(static_cast<float>(o[kOpDetune] - 7) * keep)), 14);
    }
    if (carrier) o[kOpVelocity] = clampByte(o[kOpVelocity] + m.velocity, 7);
  }
  v[kFeedbackByte] = clampByte(v[kFeedbackByte] + m.feedback, 7);
  return v;
}

}  // namespace ion::dx7
