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
inline constexpr uint8_t kMasterScope = kTrackCount;  // scope 8

enum class Curve : uint8_t {
  Linear,  // value = min + n * (max - min)
  Log,     // frequencies, Q: equal steps per octave
  Fader,   // dB = 6 + 40 log10(n), so gain = 2 n²; n = 0 is −∞
  Toggle,  // 0 or 1
  Choice,  // integer 0 … max
};

enum class Unit : uint8_t { None, Db, Hz, Pan, Percent, Semitones, Offset /* signed integer: "+12", "0" */ };

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
};

// ---- mixer parameters (per track; the master uses the same table, only Fader) ----
enum MixerParam : uint8_t {
  kFader, kPan, kMute, kSolo,
  kEqBypass,
  kHpOn, kHpFreq, kHpSlope,
  kLsOn, kLsFreq, kLsGain, kLsQ,
  kB1On, kB1Freq, kB1Gain, kB1Q,
  kB2On, kB2Freq, kB2Gain, kB2Q,
  kHsOn, kHsFreq, kHsGain, kHsQ,
  kLpOn, kLpFreq,
  kMixerParamCount
};

inline constexpr uint8_t kEngineParamBase = 64;
inline constexpr uint8_t kMaxEngineParams = 32;

struct ParamRef {
  uint8_t scope = 0;  // track 0-7, or kMasterScope
  uint8_t index = 0;  // MixerParam, or kEngineParamBase + engine param
  bool operator==(const ParamRef&) const = default;
  bool isEngine() const { return index >= kEngineParamBase; }
};

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

// Fader dB ↔ linear gain. Values at or below kFaderMinDb mean silence.
inline constexpr float kFaderMinDb = -96.0f;
float faderGain(float db);

// "track3.fader" / "master.fader" / "track3.fm.poly". The engine key prefix is
// passed in because engine tables live with the engines.
std::string paramKey(ParamRef ref, const ParamInfo& info, std::string_view enginePrefix = {});

}  // namespace ion
