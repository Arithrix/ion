#pragma once

// Every parameter the UI, the CC map and project files can address.
//
// A parameter is identified by a ParamRef: a scope (track 0-7 or the master)
// and an index into that scope's table. Mixer parameters are the same for every
// track; engine parameters come from the engine's own table and are numbered
// from kEngineParamBase. Each parameter also has a stable text key
// ("track3.eq.b1.gain") used in project files and CC maps.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ion {

inline constexpr int kTrackCount = 8;
// Scopes: the eight tracks, the master, and the two send-bus returns.
inline constexpr uint8_t kMasterScope = kTrackCount;  // 8
inline constexpr uint8_t kDelayScope = 9;             // send A's return
inline constexpr uint8_t kReverbScope = 10;           // send B's return
inline constexpr int kScopeCount = 11;

enum class Curve : uint8_t {
  Linear,  // value = min + n * (max - min)
  Log,     // frequencies, Q: equal steps per octave
  Fader,   // dB = 6 + 40 log10(n), so gain = 2 n²; n = 0 is −∞
  Toggle,  // 0 or 1
  Choice,  // integer 0 … max
};

enum class Unit : uint8_t {
  None, Db, Hz, Pan, Percent, Semitones,
  Offset,  // signed integer: "+12", "0"
  Ms,      // a time held in milliseconds: "420 ms", "1.20 s"
  Note,    // a MIDI note: "C1" (60 = C3, as on the OXI)
  Cents,   // "+12 ct"
  Ratio,   // "3.0:1"
};

struct ParamInfo {
  const char* key;    // "eq.b1.gain" — stable, used in files
  const char* label;  // "Bell 1 gain"
  float min;
  float max;
  float def;
  Curve curve;
  Unit unit;
  float fineStep;    // per ← → press: value units (Linear/Choice), normalized (Log/Fader)
  float coarseStep;  // with Shift
  const char* const* choices = nullptr;  // labels for Choice
  const char* maxLabel = nullptr;        // shown instead of the value at max ("full")
  const char* minLabel = nullptr;        // …and at min ("off")
};

// ---- mixer parameters (every scope has the strip; sends are used by tracks, and
// the delay return's Send B feeds the reverb) ----
enum MixerParam : uint8_t {
  kFader, kPan, kMute, kSolo,
  kEqBypass,
  kHpOn, kHpFreq, kHpSlope,
  kLsOn, kLsFreq, kLsGain, kLsQ,
  kB1On, kB1Freq, kB1Gain, kB1Q,
  kB2On, kB2Freq, kB2Gain, kB2Q,
  kHsOn, kHsFreq, kHsGain, kHsQ,
  kLpOn, kLpFreq,
  kSendA, kSendB, kSendPre,
  kMixerParamCount
};

// Insert slots (HANDOFF §7.2): three per scope, numbered from 64. Each slot
// has an on/bypass switch (index 0) and up to 19 parameters of its effect.
inline constexpr uint16_t kInsertBase = 64;
inline constexpr int kInsertSlots = 3;
inline constexpr int kInsertStride = 20;
// Engine parameters are numbered from 128. A drum kit has 16 pads × 20 parameters.
inline constexpr uint16_t kEngineParamBase = 128;
inline constexpr int kMaxEngineParams = 320;

struct ParamRef {
  uint8_t scope = 0;   // track 0-7, kMasterScope, kDelayScope, kReverbScope
  uint16_t index = 0;  // MixerParam, an insert parameter, or kEngineParamBase + engine param
  bool operator==(const ParamRef&) const = default;
  bool isEngine() const { return index >= kEngineParamBase; }
  bool isInsert() const { return index >= kInsertBase && index < kInsertBase + kInsertSlots * kInsertStride; }
  int slot() const { return (index - kInsertBase) / kInsertStride; }       // for isInsert()
  int slotIndex() const { return (index - kInsertBase) % kInsertStride; }  // 0 = on, then the effect's
};

// Insert slot `slot`'s on switch, and its effect's parameter `index`.
constexpr ParamRef insertOnRef(int scope, int slot) {
  return {static_cast<uint8_t>(scope), static_cast<uint16_t>(kInsertBase + slot * kInsertStride)};
}
constexpr ParamRef insertParamRef(int scope, int slot, int index) {
  return {static_cast<uint8_t>(scope), static_cast<uint16_t>(kInsertBase + slot * kInsertStride + 1 + index)};
}
// The on/bypass switch every slot has.
const ParamInfo& insertOnInfo();
// "Track 3", "Master", "Delay return"; "track3", "master", "delay" in keys.
std::string scopeLabel(int scope);
std::string scopeKey(int scope);

// Engine parameter `index` (into the engine's table) of track t.
constexpr ParamRef engineParamRef(int track, int index) {
  return {static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + index)};
}

std::span<const ParamInfo> mixerParams();
const ParamInfo& mixerParam(MixerParam p);

// Normalized 0…1 ↔ value.
float toNormalized(const ParamInfo& info, float value);
float fromNormalized(const ParamInfo& info, float normalized);
float clampValue(const ParamInfo& info, float value);
// One ← → step (direction ±1), with Shift for coarse.
float stepValue(const ParamInfo& info, float value, int direction, bool coarse);
// "−6.0 dB", "L25", "2.50 kHz", "on"
std::string formatValue(const ParamInfo& info, float value);
// "C1", "D#2": 60 is C3, as on the OXI and Yamaha gear.
std::string noteName(int note);

// Fader dB ↔ linear gain. Values at or below kFaderMinDb mean silence.
inline constexpr float kFaderMinDb = -96.0f;
float faderGain(float db);

// "track3.fader" / "master.fader" / "track3.fm.poly" / "track3.fx1.ratio". The
// engine key prefix is passed in because engine tables live with the engines.
std::string paramKey(ParamRef ref, const ParamInfo& info, std::string_view enginePrefix = {});

}  // namespace ion
