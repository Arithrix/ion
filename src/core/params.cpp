#include "core/params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace ion {
namespace {

const char* const kSlopes[] = {"12 dB", "24 dB"};

// Order must match MixerParam.
constexpr float kLogFine = 1.0f / 120.0f;  // ~1/12 octave over 20 Hz–20 kHz
constexpr float kLogCoarse = 1.0f / 20.0f;

const std::array<ParamInfo, kMixerParamCount> kMixer = {{
    {"fader", "Fader", kFaderMinDb, 6.0f, 0.0f, Curve::Fader, Unit::Db, 0.005f, 0.05f},
    {"pan", "Pan", -1.0f, 1.0f, 0.0f, Curve::Linear, Unit::Pan, 0.05f, 0.25f},
    {"mute", "Mute", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"solo", "Solo", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"eq.bypass", "EQ bypass", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"eq.hp.on", "High-pass", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"eq.hp.freq", "High-pass freq", 20.0f, 2000.0f, 40.0f, Curve::Log, Unit::Hz, kLogFine * 1.5f, kLogCoarse * 1.5f},
    {"eq.hp.slope", "High-pass slope", 0, 1, 0, Curve::Choice, Unit::None, 1, 1, kSlopes},
    {"eq.ls.on", "Low shelf", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"eq.ls.freq", "Low shelf freq", 20.0f, 2000.0f, 100.0f, Curve::Log, Unit::Hz, kLogFine * 1.5f, kLogCoarse * 1.5f},
    {"eq.ls.gain", "Low shelf gain", -18.0f, 18.0f, 0.0f, Curve::Linear, Unit::Db, 0.5f, 3.0f},
    {"eq.ls.q", "Low shelf Q", 0.3f, 2.0f, 0.707f, Curve::Log, Unit::None, kLogFine * 3, kLogCoarse * 3},
    {"eq.b1.on", "Bell 1", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"eq.b1.freq", "Bell 1 freq", 40.0f, 18000.0f, 400.0f, Curve::Log, Unit::Hz, kLogFine, kLogCoarse},
    {"eq.b1.gain", "Bell 1 gain", -18.0f, 18.0f, 0.0f, Curve::Linear, Unit::Db, 0.5f, 3.0f},
    {"eq.b1.q", "Bell 1 Q", 0.1f, 10.0f, 1.0f, Curve::Log, Unit::None, kLogFine * 1.5f, kLogCoarse * 1.5f},
    {"eq.b2.on", "Bell 2", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"eq.b2.freq", "Bell 2 freq", 40.0f, 18000.0f, 2500.0f, Curve::Log, Unit::Hz, kLogFine, kLogCoarse},
    {"eq.b2.gain", "Bell 2 gain", -18.0f, 18.0f, 0.0f, Curve::Linear, Unit::Db, 0.5f, 3.0f},
    {"eq.b2.q", "Bell 2 Q", 0.1f, 10.0f, 1.0f, Curve::Log, Unit::None, kLogFine * 1.5f, kLogCoarse * 1.5f},
    {"eq.hs.on", "High shelf", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1},
    {"eq.hs.freq", "High shelf freq", 1000.0f, 18000.0f, 8000.0f, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"eq.hs.gain", "High shelf gain", -18.0f, 18.0f, 0.0f, Curve::Linear, Unit::Db, 0.5f, 3.0f},
    {"eq.hs.q", "High shelf Q", 0.3f, 2.0f, 0.707f, Curve::Log, Unit::None, kLogFine * 3, kLogCoarse * 3},
    {"eq.lp.on", "Low-pass", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
    {"eq.lp.freq", "Low-pass freq", 1000.0f, 20000.0f, 18000.0f, Curve::Log, Unit::Hz, kLogFine * 2, kLogCoarse * 2},
    {"send.a", "Send A (delay)", kFaderMinDb, 6.0f, kFaderMinDb, Curve::Fader, Unit::Db, 0.01f, 0.05f},
    {"send.b", "Send B (reverb)", kFaderMinDb, 6.0f, kFaderMinDb, Curve::Fader, Unit::Db, 0.01f, 0.05f},
    {"send.pre", "Sends pre-fader", 0, 1, 0, Curve::Toggle, Unit::None, 1, 1},
}};

const ParamInfo kInsertOn = {"on", "On", 0, 1, 1, Curve::Toggle, Unit::None, 1, 1};

}  // namespace

std::span<const ParamInfo> mixerParams() { return kMixer; }
const ParamInfo& insertOnInfo() { return kInsertOn; }

std::string scopeLabel(int scope) {
  if (scope == kMasterScope) return "Master";
  if (scope == kDelayScope) return "Delay return";
  if (scope == kReverbScope) return "Reverb return";
  return "Track " + std::to_string(scope + 1);
}

std::string scopeKey(int scope) {
  if (scope == kMasterScope) return "master";
  if (scope == kDelayScope) return "delay";
  if (scope == kReverbScope) return "reverb";
  return "track" + std::to_string(scope + 1);
}
const ParamInfo& mixerParam(MixerParam p) { return kMixer[p]; }

float faderGain(float db) {
  if (db <= kFaderMinDb) return 0.0f;
  return std::pow(10.0f, db / 20.0f);
}

float clampValue(const ParamInfo& info, float value) {
  if (!std::isfinite(value)) return info.def;
  value = std::clamp(value, info.min, info.max);
  if (info.curve == Curve::Toggle || info.curve == Curve::Choice) value = std::round(value);
  return value;
}

float toNormalized(const ParamInfo& info, float value) {
  value = clampValue(info, value);
  switch (info.curve) {
    case Curve::Log:
      return std::log(value / info.min) / std::log(info.max / info.min);
    case Curve::Fader:
      if (value <= kFaderMinDb) return 0.0f;
      return std::clamp(std::pow(10.0f, (value - 6.0f) / 40.0f), 0.0f, 1.0f);
    default:
      return info.max > info.min ? (value - info.min) / (info.max - info.min) : 0.0f;
  }
}

float fromNormalized(const ParamInfo& info, float n) {
  n = std::clamp(n, 0.0f, 1.0f);
  switch (info.curve) {
    case Curve::Log:
      return info.min * std::pow(info.max / info.min, n);
    case Curve::Fader: {
      if (n <= 0.0f) return kFaderMinDb;
      const float db = 6.0f + 40.0f * std::log10(n);
      return std::max(db, kFaderMinDb);
    }
    default:
      return clampValue(info, info.min + n * (info.max - info.min));
  }
}

float stepValue(const ParamInfo& info, float value, int direction, bool coarse) {
  const float step = (coarse ? info.coarseStep : info.fineStep) * static_cast<float>(direction);
  switch (info.curve) {
    case Curve::Toggle:
      return value >= 0.5f ? 0.0f : 1.0f;
    case Curve::Log:
    case Curve::Fader:
      return fromNormalized(info, toNormalized(info, value) + step);
    default:
      return clampValue(info, value + step);
  }
}

std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  note = std::clamp(note, 0, 127);
  return std::string(names[note % 12]) + std::to_string(note / 12 - 2);
}

std::string formatValue(const ParamInfo& info, float v) {
  char buf[32];
  if (info.maxLabel && v >= info.max) return info.maxLabel;
  if (info.minLabel && v <= info.min) return info.minLabel;
  if (info.unit == Unit::Note) return noteName(static_cast<int>(std::lround(v)));
  if (info.unit == Unit::Cents) {
    const long n = std::lround(v);
    if (n == 0) return "0 ct";
    std::snprintf(buf, sizeof buf, "%s%ld ct", n > 0 ? "+" : "−", std::labs(n));
    return buf;
  }
  switch (info.curve) {
    case Curve::Toggle:
      return v >= 0.5f ? "on" : "off";
    case Curve::Choice:
      if (info.choices) return info.choices[static_cast<int>(std::lround(v))];
      if (info.unit == Unit::Semitones && info.min < 0) {
        const long n = std::lround(v);
        if (n == 0) return "0 st";
        std::snprintf(buf, sizeof buf, "%s%ld st", n > 0 ? "+" : "−", std::labs(n));
        return buf;
      }
      if (info.unit == Unit::Offset) {
        const long n = std::lround(v);
        if (n == 0) return "0";
        std::snprintf(buf, sizeof buf, "%s%ld", n > 0 ? "+" : "−", std::labs(n));  // a real minus sign
        return buf;
      }
      return std::to_string(static_cast<int>(std::lround(v)));
    default:
      break;
  }
  switch (info.unit) {
    case Unit::Db:
      if (info.curve == Curve::Fader && v <= kFaderMinDb) return "−∞ dB";
      std::snprintf(buf, sizeof buf, "%+.1f dB", static_cast<double>(v));
      if (std::fabs(v) < 0.05f) return "0.0 dB";
      break;
    case Unit::Hz:
      if (v >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f kHz", static_cast<double>(v / 1000.0f));
      else if (v < 10.0f) std::snprintf(buf, sizeof buf, "%.2f Hz", static_cast<double>(v));
      else std::snprintf(buf, sizeof buf, "%.0f Hz", static_cast<double>(v));
      break;
    case Unit::Pan: {
      const int p = static_cast<int>(std::lround(v * 100.0f));
      if (p == 0) return "C";
      std::snprintf(buf, sizeof buf, "%s%d", p < 0 ? "L" : "R", std::abs(p));
      break;
    }
    case Unit::Percent:
      std::snprintf(buf, sizeof buf, "%.0f%%", static_cast<double>(v * 100.0f));
      break;
    case Unit::Semitones:
      std::snprintf(buf, sizeof buf, "%+.0f st", static_cast<double>(v));
      break;
    case Unit::Ms:
      if (v >= 1000.0f) std::snprintf(buf, sizeof buf, "%.2f s", static_cast<double>(v / 1000.0f));
      else if (v >= 100.0f) std::snprintf(buf, sizeof buf, "%.0f ms", static_cast<double>(v));
      else std::snprintf(buf, sizeof buf, "%.1f ms", static_cast<double>(v));
      break;
    case Unit::Ratio:
      std::snprintf(buf, sizeof buf, v >= 10.0f ? "%.0f:1" : "%.1f:1", static_cast<double>(v));
      break;
    case Unit::None:
    case Unit::Offset:
    case Unit::Note:
    case Unit::Cents:
      std::snprintf(buf, sizeof buf, info.max - info.min > 20 ? "%.0f" : "%.2f", static_cast<double>(v));
      break;
  }
  return buf;
}

std::string paramKey(ParamRef ref, const ParamInfo& info, std::string_view enginePrefix) {
  std::string key = scopeKey(ref.scope);
  key += '.';
  if (ref.isInsert()) key += "fx" + std::to_string(ref.slot() + 1) + ".";
  if (ref.isEngine()) {
    key += enginePrefix;
    key += '.';
  }
  key += info.key;
  return key;
}

}  // namespace ion
