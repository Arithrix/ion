#include "project/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#include "core/version.h"
#include "engine/drum/drum_engine.h"
#include "engine/envelope_params.h"
#include "engine/fm/fm_engine.h"
#include "engine/acid/acid_engine.h"
#include "engine/chip/chip_engine.h"
#include "engine/plaits/plaits_engine.h"
#include "engine/sampler/sampler_engine.h"

namespace ion {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string toHex(const uint8_t* data, std::size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s += digits[data[i] >> 4];
    s += digits[data[i] & 15];
  }
  return s;
}

bool fromHex(const std::string& s, uint8_t* out, std::size_t n) {
  if (s.size() != n * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < n; ++i) {
    const int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

std::string nowIso() {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
  return buf;
}

ParamRef mixerRef(int scope, int index) { return {static_cast<uint8_t>(scope), static_cast<uint8_t>(index)}; }
ParamRef engineRef(int track, int index) {
  return {static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + index)};
}

// Brings an older project file up to kProjectFormat. Each bump adds a step.
//   1 → 2: FM macros and the program list were added. Both are optional
//          fields with defaults, so a format-1 file reads as it is.
//   2 → 3: drum tracks (pads, kit name). Older files have none: nothing to do.
//   3 → 4: inserts, sends, returns; the master's whole strip. Older files keep
//          the defaults (no inserts, sends off, the glue compressor out), and
//          the master's "fader" is still read.
//   4 → 5: sampler tracks (nothing to convert), and the uniform envelope
//          (D-50): drum pads' "vel.level" is now "vel.amp"; hold "full" moved
//          from 5000 to 10000; a gate pad's decay is now its release. FM's
//          Attack and Release macros are gone (their values and CCs drop).
//   5 → 6: the Rec setup ("rec"), optional: older files get the defaults.
//   6 → 7: Plaits tracks ("plaits", with its patch list). Nothing to convert.
//   7 → 8: each track's modulation ("mod"), optional: older tracks keep the defaults, routed nowhere.
//   8 → 9: ACID tracks and Drive inserts. Nothing to convert; the bump stops
//          0.8 from opening a file it would load with those tracks and slots empty.
//   9 → 10: CHIP tracks; nothing to convert (the same reason as 9).
// A drum pad's settings from before the uniform envelope (0.3, 0.4), in place.
// `prefix` is "padN." in a project, "" in a kit.
void upgradePadParams(json& params, const std::string& prefix) {
  if (!params.is_object()) return;
  auto rename = [&](const std::string& from, const std::string& to) {
    if (!params.contains(prefix + from)) return;
    params[prefix + to] = params[prefix + from];
    params.erase(prefix + from);
  };
  rename("vel.level", "vel.amp");
  rename("hold", "amp.hold");
  rename("decay", "amp.decay");
  const std::string hold = prefix + "amp.hold", decay = prefix + "amp.decay", mode = prefix + "mode";
  if (params.contains(hold) && params[hold].is_number() && params[hold].get<float>() >= 5000.0f) params[hold] = kEnvFull;
  if (params.contains(mode) && params[mode].is_number() && params[mode].get<float>() >= 0.5f) {
    // Gate used to hold while the key was down and decay from note-off.
    params[prefix + "amp.release"] = params.contains(decay) ? params[decay] : json(500.0);
    params[hold] = kEnvFull;
  }
}

bool migrate(json& j, std::string& error) {
  if (!j.contains("format") || !j["format"].is_number_integer()) {
    error = "not an ion project (no \"format\")";
    return false;
  }
  const int format = j["format"].get<int>();
  if (format > kProjectFormat) {
    error = "saved by a newer ion (format " + std::to_string(format) + "); this version reads format " +
            std::to_string(kProjectFormat);
    return false;
  }
  if (format < 1) {
    error = "unknown project format " + std::to_string(format);
    return false;
  }
  if (format < 5) {
    if (j.contains("tracks") && j["tracks"].is_array())
      for (json& jt : j["tracks"])
        if (jt.value("engine", std::string()) == "drum" && jt.contains("drum") && jt["drum"].contains("params"))
          for (int pad = 1; pad <= DrumEngine::kPads; ++pad) upgradePadParams(jt["drum"]["params"], "pad" + std::to_string(pad) + ".");
    if (j.contains("cc") && j["cc"].is_array())
      for (json& jc : j["cc"]) {
        std::string target = jc.value("target", std::string());
        const auto at = target.find(".vel.level");
        if (at != std::string::npos && target.find(".drum.pad") != std::string::npos) jc["target"] = target.replace(at, 10, ".vel.amp");
      }
  }
  return true;
}

}  // namespace

std::unique_ptr<Engine> createEngine(EngineType type) {
  switch (type) {
    case EngineType::Fm: return std::make_unique<FmEngine>();
    case EngineType::Drum: return std::make_unique<DrumEngine>();
    case EngineType::Sampler: return std::make_unique<SamplerEngine>();
    case EngineType::Plaits: return std::make_unique<PlaitsEngine>();
    case EngineType::Acid: return std::make_unique<AcidEngine>();
    case EngineType::Chip: return std::make_unique<ChipEngine>();
    default: return nullptr;
  }
}

Session::Session(Module& module, Paths paths, Config& config)
    : module_(module), paths_(std::move(paths)), config_(config), loader_(std::make_unique<SampleLoader>()) {
  for (int t = 0; t < kTrackCount; ++t) tracks_[static_cast<std::size_t>(t)].channel = t;
  inserts_[kDelayScope][0] = EffectType::Delay;
  inserts_[kReverbScope][0] = EffectType::Reverb;
  defaultValues();
  lastStore_ = values_;
  saved_ = autosaved_ = values_;
  clearHistory();
  Command c;
  c.type = Command::Type::SetTiming;
  c.value = config_.timing == MidiTiming::Immediate ? 1.0f : 0.0f;
  send(c);
}

Session::~Session() {
  // Retired engines still waiting in the queue are freed by the Module. A
  // take still in flight may yet be written by the audio thread: it leaks
  // rather than being freed under it.
  (void)take_.release();
}

void Session::defaultValues() {
  values_.fill(0.0f);
  for (int s = 0; s < ParamStore::kScopes; ++s)
    for (int i = 0; i < kMixerParamCount; ++i)
      values_[static_cast<std::size_t>(ParamStore::index(mixerRef(s, i)))] = mixerParams()[static_cast<std::size_t>(i)].def;
  for (int t = 0; t < kTrackCount; ++t)
    for (int i = 0; i < kModParamCount; ++i)
      values_[static_cast<std::size_t>(ParamStore::index({static_cast<uint8_t>(t), static_cast<uint16_t>(kModBase + i)}))] =
          modParams()[static_cast<std::size_t>(i)].def;
  // The returns' own effects (the Module makes them at these defaults).
  for (int scope : {kDelayScope, kReverbScope}) {
    values_[static_cast<std::size_t>(ParamStore::index(insertOnRef(scope, 0)))] = 1;
    const auto table = effectParams(inserts_[static_cast<std::size_t>(scope)][0]);
    for (std::size_t i = 0; i < table.size(); ++i)
      values_[static_cast<std::size_t>(ParamStore::index(insertParamRef(scope, 0, static_cast<int>(i))))] = table[i].def;
  }
}

// ---- effects ---------------------------------------------------------------------------

bool Session::insertFixed(int scope, int slot) const { return scope >= kDelayScope && slot == 0; }

EffectType Session::insert(int scope, int slot) const {
  if (scope < 0 || scope >= kScopeCount || slot < 0 || slot >= kInsertSlots) return EffectType::None;
  return inserts_[static_cast<std::size_t>(scope)][static_cast<std::size_t>(slot)];
}

void Session::setInsert(int scope, int slot, EffectType type) {
  if (scope < 0 || scope >= kScopeCount || slot < 0 || slot >= kInsertSlots || insertFixed(scope, slot)) return;
  editLabel(scopeLabel(scope) + " · insert " + std::to_string(slot + 1) + ": " + (type == EffectType::None ? "empty" : effectLabel(type)));
  if (scope < kTrackCount) dropModTargets(scope, false, slot);
  inserts_[static_cast<std::size_t>(scope)][static_cast<std::size_t>(slot)] = type;
  Command c;
  c.type = Command::Type::SetInsert;
  c.scope = static_cast<uint8_t>(scope);
  c.slot = slot;
  c.effect = createEffect(type).release();
  send(c);
  // The Module resets the slot to the effect's defaults, switched in.
  for (int i = 0; i < kInsertStride; ++i)
    values_[static_cast<std::size_t>(ParamStore::index({static_cast<uint8_t>(scope), static_cast<uint16_t>(kInsertBase + slot * kInsertStride + i)}))] = 0;
  values_[static_cast<std::size_t>(ParamStore::index(insertOnRef(scope, slot)))] = 1;
  const auto table = effectParams(type);
  for (std::size_t i = 0; i < table.size(); ++i)
    values_[static_cast<std::size_t>(ParamStore::index(insertParamRef(scope, slot, static_cast<int>(i))))] = table[i].def;
  markDirty();
}

void Session::moveInsert(int scope, int slot, int delta) {
  const int to = slot + delta;
  if (to < 0 || to >= kInsertSlots || insertFixed(scope, slot) || insertFixed(scope, to)) return;
  auto settings = [&](int sl) {
    std::array<float, kInsertStride> v{};
    for (int i = 0; i < kInsertStride; ++i) v[static_cast<std::size_t>(i)] = param({static_cast<uint8_t>(scope), static_cast<uint16_t>(kInsertBase + sl * kInsertStride + i)});
    return v;
  };
  const EffectType a = insert(scope, slot), b = insert(scope, to);
  editLabel(scopeLabel(scope) + " · move insert " + std::to_string(slot + 1) + " → " + std::to_string(to + 1));
  const auto va = settings(slot), vb = settings(to);
  auto place = [&](int sl, EffectType type, const std::array<float, kInsertStride>& v) {
    setInsert(scope, sl, type);
    if (type == EffectType::None) return;
    for (int i = 0; i < kInsertStride; ++i) {
      const ParamRef ref{static_cast<uint8_t>(scope), static_cast<uint16_t>(kInsertBase + sl * kInsertStride + i)};
      if (paramInfo(ref) && param(ref) != v[static_cast<std::size_t>(i)]) setParam(ref, v[static_cast<std::size_t>(i)]);
    }
  };
  // Routes onto either slot follow their effect.
  std::array<int16_t, kModRoutes> targets{};
  if (scope < kTrackCount) targets = tracks_[static_cast<std::size_t>(scope)].modTargets;
  place(to, a, va);
  place(slot, b, vb);
  if (scope < kTrackCount)
    for (int r = 0; r < kModRoutes; ++r) {
      const int16_t was = targets[static_cast<std::size_t>(r)];
      if (was < kInsertBase || was >= kInsertBase + kInsertSlots * kInsertStride) continue;
      const int from = (was - kInsertBase) / kInsertStride;
      if (from != slot && from != to) continue;
      const int16_t now = static_cast<int16_t>(was + (from == slot ? to - slot : slot - to) * kInsertStride);
      tracks_[static_cast<std::size_t>(scope)].modTargets[static_cast<std::size_t>(r)] = now;
      sendModTarget(scope, r);
    }
}

void Session::send(const Command& c) {
  // The audio thread drains the queue every 1.3 ms; wait briefly rather than drop.
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (module_.pushCommand(c)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (c.type == Command::Type::SetEngine) delete c.engine;
  if (c.type == Command::Type::SetPrograms) delete c.programs;
  if (c.type == Command::Type::SetPlaitsBank) delete c.plaitsBank;
  if (c.type == Command::Type::SetSample || c.type == Command::Type::Preview) unpin(c.sample);
  message_ = "audio engine not responding: an edit was lost";
}

void Session::setValue(ParamRef ref, float value) {
  const auto i = static_cast<std::size_t>(ParamStore::index(ref));
  values_[i] = value;
  Command c;
  c.type = Command::Type::SetParam;
  c.param = ref;
  c.value = value;
  send(c);
}

// ---- tracks -------------------------------------------------------------------

void Session::setEngine(int t, EngineType type) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  editLabel("Track " + std::to_string(t + 1) + " · " + (type == EngineType::None ? std::string("cleared") : std::string("engine ") + engineLabel(type)));
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = static_cast<uint8_t>(t);
  c.engine = createEngine(type).release();
  if (!c.engine) type = EngineType::None;
  send(c);
  tr.engine = type;
  // A new engine starts at its defaults (the Module resets them on hand-over).
  for (int i = 0; i < kMaxEngineParams; ++i) values_[static_cast<std::size_t>(ParamStore::index(engineRef(t, i)))] = 0.0f;
  const auto table = engineParams(type);
  for (std::size_t i = 0; i < table.size(); ++i)
    values_[static_cast<std::size_t>(ParamStore::index(engineRef(t, static_cast<int>(i))))] = table[i].def;
  if (type == EngineType::Fm && tr.hasVoice) {
    Command v;
    v.type = Command::Type::LoadVoice;
    v.track = static_cast<uint8_t>(t);
    v.voice = tr.voice;
    send(v);
  }
  if (type != EngineType::Fm) {
    tr.hasVoice = false;
    tr.voiceSource.clear();
    tr.libraryId = 0;
  }
  if (type == EngineType::Plaits) sendPlaitsBank(t);
  else tr.plaitsPatches.clear();
  dropModTargets(t, true, -1);  // the old engine's values are gone
  // The new engine has empty pads: load the track's samples onto it (or, for
  // another engine, forget them).
  padLoads_[static_cast<std::size_t>(t)] = {};
  if (type == EngineType::Drum) {
    for (int pad = 0; pad < DrumEngine::kPads; ++pad) requestPad(t, pad);
  } else if (type == EngineType::Sampler) {
    // A sampler has one sample (pad 0 here).
    for (int pad = 1; pad < DrumEngine::kPads; ++pad) tr.pads[static_cast<std::size_t>(pad)] = {};
    tr.kit.clear();
    requestPad(t, 0);
  } else {
    tr.pads = {};
    tr.kit.clear();
  }
  markDirty();
}

void Session::setVoice(int t, const syx::Voice& voice, const std::string& source) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  editLabel("Track " + std::to_string(t + 1) + " · patch " + syx::name(voice));
  tr.hasVoice = true;
  tr.voice = syx::normalize(voice);
  tr.voiceSource = source;
  linkLibrary(tr);
  loadedAt_[static_cast<std::size_t>(t)] = std::chrono::steady_clock::now();
  if (tr.engine == EngineType::Fm) {
    Command c;
    c.type = Command::Type::LoadVoice;
    c.track = static_cast<uint8_t>(t);
    c.voice = voice;
    send(c);
  }
  markDirty();
}

void Session::setChannel(int t, int channel) {
  channel = std::clamp(channel, 0, 15);
  editLabel("Track " + std::to_string(t + 1) + " · channel " + std::to_string(channel + 1));
  tracks_[static_cast<std::size_t>(t)].channel = channel;
  Command c;
  c.type = Command::Type::SetChannel;
  c.track = static_cast<uint8_t>(t);
  c.value = static_cast<float>(channel);
  send(c);
  markDirty();
}

// ---- parameters ----------------------------------------------------------------

const ParamInfo* Session::paramInfo(ParamRef ref) const {
  if (ref.scope >= ParamStore::kScopes) return nullptr;
  if (ref.isInsert()) {
    const EffectType type = inserts_[ref.scope][static_cast<std::size_t>(ref.slot())];
    if (type == EffectType::None) return nullptr;
    if (ref.slotIndex() == 0) return &insertOnInfo();
    const auto table = effectParams(type);
    const std::size_t i = static_cast<std::size_t>(ref.slotIndex() - 1);
    return i < table.size() ? &table[i] : nullptr;
  }
  if (ref.isMod()) {
    const std::size_t i = ref.index - kModBase;
    return ref.scope < kTrackCount && i < static_cast<std::size_t>(kModParamCount) ? &modParams()[i] : nullptr;
  }
  if (!ref.isEngine()) return ref.index < kMixerParamCount ? &mixerParams()[ref.index] : nullptr;
  if (ref.scope >= kTrackCount) return nullptr;
  const auto table = engineParams(tracks_[ref.scope].engine);
  const std::size_t i = ref.index - kEngineParamBase;
  return i < table.size() ? &table[i] : nullptr;
}

void Session::setParam(ParamRef ref, float value) {
  const ParamInfo* pi = paramInfo(ref);
  if (!pi) return;
  setValue(ref, clampValue(*pi, value));
}

void Session::nudgeParam(ParamRef ref, int direction, bool coarse) {
  const ParamInfo* pi = paramInfo(ref);
  if (!pi) return;
  setValue(ref, stepValue(*pi, param(ref), direction, coarse));
}

std::string Session::paramLabel(ParamRef ref) const {
  const ParamInfo* pi = paramInfo(ref);
  std::string s = scopeLabel(ref.scope) + " · ";
  if (ref.isInsert()) {
    s += std::string(effectLabel(inserts_[ref.scope][static_cast<std::size_t>(ref.slot())])) + " " +
         std::to_string(ref.slot() + 1) + " ";
  }
  if (ref.isEngine() && ref.scope < kTrackCount) {
    s += engineLabel(tracks_[ref.scope].engine);
    s += ' ';
  }
  if (ref.isMod()) {
    // "LFO 1 rate", "Env 2 decay", "Mod 3 depth"
    const int i = ref.index - kModBase;
    if (i < kLfos * kLfoParamCount) s += "LFO " + std::to_string(i / kLfoParamCount + 1) + " ";
    else if (i < kLfos * kLfoParamCount + kModEnvs * kModEnvParamCount)
      s += "Env " + std::to_string((i - kLfos * kLfoParamCount) / kModEnvParamCount + 1) + " ";
    else s += "Mod " + std::to_string((i - kLfos * kLfoParamCount - kModEnvs * kModEnvParamCount) / kRouteParamCount + 1) + " ";
    if (pi) {
      std::string l = pi->label;
      l[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(l[0])));
      return s + l;
    }
  }
  s += pi ? pi->label : "?";
  return s;
}

std::string Session::paramKey(ParamRef ref) const {
  const ParamInfo* pi = paramInfo(ref);
  if (!pi) return {};
  const char* prefix = ref.isEngine() && ref.scope < kTrackCount ? engineKey(tracks_[ref.scope].engine) : "";
  return ion::paramKey(ref, *pi, prefix);
}

std::optional<ParamRef> Session::parseParamKey(const std::string& key) const {
  int scope = -1;
  std::string rest;
  for (int named : {static_cast<int>(kMasterScope), static_cast<int>(kDelayScope), static_cast<int>(kReverbScope)}) {
    const std::string prefix = scopeKey(named) + ".";
    if (key.rfind(prefix, 0) == 0) {
      scope = named;
      rest = key.substr(prefix.size());
    }
  }
  if (scope >= 0) {
  } else if (key.rfind("track", 0) == 0) {
    const auto dot = key.find('.');
    if (dot == std::string::npos) return std::nullopt;
    scope = std::atoi(key.substr(5, dot - 5).c_str()) - 1;
    if (scope < 0 || scope >= kTrackCount) return std::nullopt;
    rest = key.substr(dot + 1);
  } else {
    return std::nullopt;
  }
  for (int i = 0; i < kMixerParamCount; ++i)
    if (rest == mixerParams()[static_cast<std::size_t>(i)].key) return mixerRef(scope, i);
  if (scope < kTrackCount)
    for (int i = 0; i < kModParamCount; ++i)
      if (rest == modParams()[static_cast<std::size_t>(i)].key)
        return ParamRef{static_cast<uint8_t>(scope), static_cast<uint16_t>(kModBase + i)};
  // "fx2.ratio", "fx1.on"
  if (rest.size() > 4 && rest.rfind("fx", 0) == 0 && rest[3] == '.' && rest[2] >= '1' && rest[2] < '1' + kInsertSlots) {
    const int slot = rest[2] - '1';
    const std::string name = rest.substr(4);
    if (name == insertOnInfo().key) return insertOnRef(scope, slot);
    const auto table = effectParams(inserts_[static_cast<std::size_t>(scope)][static_cast<std::size_t>(slot)]);
    for (std::size_t i = 0; i < table.size(); ++i)
      if (name == table[i].key) return insertParamRef(scope, slot, static_cast<int>(i));
    return std::nullopt;
  }
  if (scope < kTrackCount) {
    const EngineType type = tracks_[static_cast<std::size_t>(scope)].engine;
    const std::string prefix = std::string(engineKey(type)) + ".";
    if (rest.rfind(prefix, 0) == 0) {
      const auto table = engineParams(type);
      for (std::size_t i = 0; i < table.size(); ++i)
        if (rest.substr(prefix.size()) == table[i].key) return engineRef(scope, static_cast<int>(i));
    }
  }
  return std::nullopt;
}

// ---- CC map ------------------------------------------------------------------------

std::vector<Session::CcEntry> Session::ccMappings() const {
  std::vector<CcEntry> out;
  for (int i = 0; i < kMaxCcMappings; ++i)
    if (cc_[static_cast<std::size_t>(i)].used) out.push_back({i, cc_[static_cast<std::size_t>(i)]});
  std::sort(out.begin(), out.end(), [](const CcEntry& a, const CcEntry& b) {
    return a.mapping.cc != b.mapping.cc ? a.mapping.cc < b.mapping.cc : a.mapping.channel < b.mapping.channel;
  });
  return out;
}

void Session::learn(ParamRef ref) {
  learning_ = ref;
  Command c;
  c.type = Command::Type::LearnCc;
  c.param = ref;
  send(c);
}

void Session::cancelLearn() {
  if (!learning_) return;
  learning_.reset();
  Command c;
  c.type = Command::Type::CancelLearn;
  send(c);
}

int Session::addMapping(uint8_t channel, uint8_t cc, ParamRef target, float lo, float hi) {
  // One control drives one parameter: drop whatever this CC or this parameter
  // was mapped to before.
  for (int i = 0; i < kMaxCcMappings; ++i) {
    const CcMapping& m = cc_[static_cast<std::size_t>(i)];
    if (m.used && ((m.cc == cc && m.channel == channel) || m.target == target)) removeMapping(i);
  }
  editLabel("CC map: cc " + std::to_string(cc) + " → " + paramLabel(target));
  for (int i = 0; i < kMaxCcMappings; ++i) {
    if (cc_[static_cast<std::size_t>(i)].used) continue;
    const CcMapping m{true, channel, cc, target, std::clamp(lo, 0.0f, 1.0f), std::clamp(hi, 0.0f, 1.0f)};
    cc_[static_cast<std::size_t>(i)] = m;
    Command c;
    c.type = Command::Type::SetCcMapping;
    c.slot = i;
    c.mapping = m;
    send(c);
    markDirty();
    return i;
  }
  return -1;
}

void Session::removeMapping(int slot) {
  if (slot < 0 || slot >= kMaxCcMappings) return;
  editLabel("CC map: remove cc " + std::to_string(cc_[static_cast<std::size_t>(slot)].cc));
  cc_[static_cast<std::size_t>(slot)] = {};
  Command c;
  c.type = Command::Type::SetCcMapping;
  c.slot = slot;
  c.mapping = {};
  send(c);
  markDirty();
}

void Session::setMappingRange(int slot, float lo, float hi) {
  if (slot < 0 || slot >= kMaxCcMappings || !cc_[static_cast<std::size_t>(slot)].used) return;
  CcMapping& m = cc_[static_cast<std::size_t>(slot)];
  editLabel("CC map: cc " + std::to_string(m.cc) + " range");
  m.lo = std::clamp(lo, 0.0f, 1.0f);
  m.hi = std::clamp(hi, 0.0f, 1.0f);
  Command c;
  c.type = Command::Type::SetCcMapping;
  c.slot = slot;
  c.mapping = m;
  send(c);
  markDirty();
}

// ---- playing -----------------------------------------------------------------------

void Session::sendMidi(const MidiMessage& m) {
  Command c;
  c.type = Command::Type::Midi;
  c.midi = m;
  send(c);
}

void Session::setTiming(MidiTiming timing) {
  config_.timing = timing;
  Command c;
  c.type = Command::Type::SetTiming;
  c.value = timing == MidiTiming::Immediate ? 1.0f : 0.0f;
  send(c);
}

void Session::panic() {
  Command c;
  c.type = Command::Type::Panic;
  send(c);
}

// ---- the FM library ---------------------------------------------------------------

bool Session::openLibrary(std::string& error) {
  library_ = FmLibrary::open(paths_.libraryFile(), error);
  if (!library_) return false;
  for (TrackState& tr : tracks_) linkLibrary(tr);
  return true;
}

void Session::linkLibrary(TrackState& tr) {
  const FmPatch* p = library_ && tr.hasVoice ? library_->findVoice(tr.voice) : nullptr;
  tr.libraryId = p ? p->id : 0;
}

void Session::loadPatch(int t, const FmPatch& patch) {
  setVoice(t, patch.voice, patch.source);
  tracks_[static_cast<std::size_t>(t)].libraryId = patch.id;
}

dx7::Macros Session::macros(int t) const {
  auto v = [&](int i) { return static_cast<int>(std::lround(param(engineRef(t, FmEngine::kFirstMacro + i)))); };
  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Fm) return {};
  return {v(0), v(1), v(2), v(3)};
}

void Session::resetMacros(int t) {
  editLabel("Track " + std::to_string(t + 1) + " · reset macros");
  for (int i = 0; i < dx7::kMacroCount; ++i) setParam(engineRef(t, FmEngine::kFirstMacro + i), 0.0f);
}

int64_t Session::savePatchAs(int t, const std::string& name, std::string& error) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Fm || !tr.hasVoice) {
    error = "no FM patch on this track";
    return 0;
  }
  if (!library_) {
    error = "the library isn't open";
    return 0;
  }
  syx::Voice baked = dx7::applyMacros(tr.voice, macros(t));
  // DX7 names are 10 printable ASCII characters.
  std::string n = name.substr(0, syx::kNameLength);
  n.resize(syx::kNameLength, ' ');
  for (std::size_t i = 0; i < syx::kNameLength; ++i) {
    const auto c = static_cast<unsigned char>(n[i]);
    baked[syx::kNameOffset + i] = (c >= 32 && c <= 126) ? c : static_cast<uint8_t>(' ');
  }
  const bool existed = library_->findVoice(baked) != nullptr;
  const int64_t id = library_->addUser(baked, {}, error);
  if (!id) return 0;
  const FmPatch* p = library_->find(id);
  // The track now plays the baked voice with the macros at zero: it sounds
  // exactly the same, and "edited" goes away.
  resetMacros(t);
  if (p) loadPatch(t, *p);
  message_ = existed && p ? "already in the library as " + p->name : "saved to the library: " + syx::name(baked);
  return id;
}

bool Session::exportVoices(const std::vector<syx::Voice>& voices, const std::string& name,
                           std::vector<fs::path>& written, std::string& error) {
  if (voices.empty()) {
    error = "nothing to export";
    return false;
  }
  if (!validProjectName(name)) {
    error = "names can use letters, digits, spaces, - _ and .";
    return false;
  }
  const auto banks = syx::makeBanks(voices);
  for (std::size_t i = 0; i < banks.size(); ++i) {
    const std::string file = banks.size() == 1 ? name + ".syx" : name + " " + std::to_string(i + 1) + ".syx";
    const fs::path path = paths_.exportsDir / file;
    std::error_code ec;
    if (fs::exists(path, ec)) {
      error = "\"" + file + "\" already exists in " + paths_.exportsDir.string();
      return false;
    }
    if (!writeFileAtomic(path, std::string(banks[i].begin(), banks[i].end()), error)) return false;
    written.push_back(path);
  }
  return true;
}

// ---- program change -------------------------------------------------------------------

int Session::programIndex(const syx::Voice& voice) const {
  const uint64_t h = fmVoiceHash(syx::normalize(voice));
  for (std::size_t i = 0; i < programs_.size(); ++i)
    if (fmVoiceHash(programs_[i].voice) == h) return static_cast<int>(i);
  return -1;
}

bool Session::toggleProgram(const syx::Voice& voice, const std::string& source) {
  editLabel("program list: " + syx::name(voice));
  if (const int i = programIndex(voice); i >= 0) {
    programs_.erase(programs_.begin() + i);
  } else {
    if (static_cast<int>(programs_.size()) >= kMaxPrograms) return false;
    programs_.push_back({syx::normalize(voice), source});
  }
  sendPrograms();
  markDirty();
  return true;
}

void Session::moveProgram(int index, int delta) {
  const int to = index + delta;
  if (index < 0 || to < 0 || index >= static_cast<int>(programs_.size()) || to >= static_cast<int>(programs_.size())) return;
  editLabel("program list: move " + syx::name(programs_[static_cast<std::size_t>(index)].voice));
  std::swap(programs_[static_cast<std::size_t>(index)], programs_[static_cast<std::size_t>(to)]);
  sendPrograms();
  markDirty();
}

void Session::setProgramChange(bool on) {
  config_.programChange = on;
  sendPrograms();
}

void Session::sendPrograms() {
  Command c;
  c.type = Command::Type::SetPrograms;
  if (config_.programChange && !programs_.empty()) {
    c.programs = new ProgramTable();
    c.programs->count = static_cast<int>(programs_.size());
    for (std::size_t i = 0; i < programs_.size(); ++i) c.programs->voices[i] = programs_[i].voice;
  }
  send(c);
}

// ---- drum kits -------------------------------------------------------------------------

fs::path Session::resolveSample(const std::string& stored) const {
  if (stored.empty()) return {};
  const fs::path p = expandHome(stored);
  return p.is_absolute() || name_.empty() ? p : paths_.projectDir(name_) / p;
}

void Session::setPadSample(int t, int pad, const std::string& path) {
  if (t < 0 || t >= kTrackCount || pad < 0 || pad >= DrumEngine::kPads) return;
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Drum && !(tr.engine == EngineType::Sampler && pad == 0)) return;
  editLabel("Track " + std::to_string(t + 1) + (tr.engine == EngineType::Sampler ? std::string(" · sample") : " · pad " + std::to_string(pad + 1)) +
            (path.empty() ? std::string(" cleared") : ": " + fs::path(path).stem().string()));
  tr.pads[static_cast<std::size_t>(pad)].sample = path;
  requestPad(t, pad);
  markDirty();
}

void Session::requestPad(int t, int pad) {
  const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  PadLoad& pl = padLoads_[static_cast<std::size_t>(t)][static_cast<std::size_t>(pad)];
  pl.tag = nextTag_++;
  pl.error.clear();
  const std::string& stored = tr.pads[static_cast<std::size_t>(pad)].sample;
  if (stored.empty()) {
    pl.loading = false;
    if (pl.sample) sendSample(t, pad, nullptr);
    pl.sample = nullptr;
    return;
  }
  pl.loading = true;
  loader_->request(resolveSample(stored), pl.tag);
}

void Session::sendSample(int t, int pad, const SampleLoader::Ptr& sample) {
  const EngineType type = tracks_[static_cast<std::size_t>(t)].engine;
  if (type != EngineType::Drum && type != EngineType::Sampler) return;
  pin(sample);
  Command c;
  c.type = Command::Type::SetSample;
  c.track = static_cast<uint8_t>(t);
  c.slot = pad;
  c.sample = sample.get();
  send(c);
}

void Session::pin(const SampleLoader::Ptr& sample) {
  if (sample) pinned_.push_back(sample);
}

void Session::unpin(const Sample* sample) {
  if (!sample) return;
  const auto it = std::find_if(pinned_.begin(), pinned_.end(), [&](const SampleLoader::Ptr& p) { return p.get() == sample; });
  if (it != pinned_.end()) pinned_.erase(it);
}

void Session::handleLoads() {
  for (SampleLoader::Result& r : loader_->poll()) {
    if (r.preview) {
      if (r.tag != previewTag_) continue;  // the cursor has moved on
      if (!r.sample) {
        previewError_ = r.error;
        continue;
      }
      recentPreviews_.push_front(r.sample);
      if (recentPreviews_.size() > 8) recentPreviews_.pop_back();
      if (previewPlay_) {
        pin(r.sample);
        Command c;
        c.type = Command::Type::Preview;
        c.sample = r.sample.get();
        c.value = faderGain(static_cast<float>(config_.previewLevelDb));
        send(c);
      }
      continue;
    }
    for (int t = 0; t < kTrackCount; ++t)
      for (int pad = 0; pad < DrumEngine::kPads; ++pad) {
        PadLoad& pl = padLoads_[static_cast<std::size_t>(t)][static_cast<std::size_t>(pad)];
        if (pl.tag != r.tag || !pl.loading) continue;
        pl.loading = false;
        pl.error = r.error;
        // A failed load silences the pad rather than leaving the old sample
        // playing under a name it no longer has.
        if (r.sample || pl.sample) sendSample(t, pad, r.sample);
        pl.sample = r.sample;
      }
  }
}

Session::PadStatus Session::padStatus(int t, int pad) const {
  const PadLoad& pl = padLoads_[static_cast<std::size_t>(t)][static_cast<std::size_t>(pad)];
  if (tracks_[static_cast<std::size_t>(t)].pads[static_cast<std::size_t>(pad)].sample.empty()) return PadStatus::Empty;
  if (pl.loading) return PadStatus::Loading;
  return pl.sample ? PadStatus::Ready : PadStatus::Failed;
}

const std::string& Session::padError(int t, int pad) const {
  return padLoads_[static_cast<std::size_t>(t)][static_cast<std::size_t>(pad)].error;
}

SampleLoader::Ptr Session::padSample(int t, int pad) const {
  return padLoads_[static_cast<std::size_t>(t)][static_cast<std::size_t>(pad)].sample;
}

std::string Session::padName(int t, int pad) const {
  const std::string& stored = tracks_[static_cast<std::size_t>(t)].pads[static_cast<std::size_t>(pad)].sample;
  return stored.empty() ? std::string() : fs::path(stored).stem().string();
}

int Session::samplesLoading() const {
  int n = 0;
  for (const auto& track : padLoads_)
    for (const PadLoad& pl : track) n += pl.loading;
  return n;
}

bool Session::waitForSamples(int timeoutMs) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  // Pad loads and any preview in flight.
  while (samplesLoading() > 0 || loader_->pending() > 0) {
    if (std::chrono::steady_clock::now() > until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    handleLoads();
  }
  handleLoads();
  return true;
}

void Session::preview(const fs::path& path, bool play) {
  previewTag_ = nextTag_++;
  previewPlay_ = play;
  previewPath_ = path;
  previewError_.clear();
  loader_->request(path, previewTag_, true);
}

void Session::stopPreview() {
  previewTag_ = 0;
  Command c;
  c.type = Command::Type::Preview;
  send(c);
}

SampleLoader::Ptr Session::previewed(const fs::path& path) const {
  for (const SampleLoader::Ptr& p : recentPreviews_)
    if (p->path == path) return p;
  return nullptr;
}

std::string Session::previewError(const fs::path& path) const { return path == previewPath_ ? previewError_ : std::string(); }

namespace {

bool sameContent(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}

}  // namespace

int Session::collectSamples(std::string& error) {
  if (name_.empty()) {
    error = "the project has no name";
    return -1;
  }
  editLabel("collect samples");
  const fs::path dir = paths_.projectDir(name_) / "samples";
  std::map<fs::path, std::string> copied;  // source → new stored path
  int count = 0, missing = 0;
  std::error_code ec;
  for (TrackState& tr : tracks_) {
    if (tr.engine != EngineType::Drum && tr.engine != EngineType::Sampler) continue;
    for (PadState& pad : tr.pads) {
      if (pad.sample.empty() || !expandHome(pad.sample).is_absolute()) continue;  // empty, or already in the project
      const fs::path src = resolveSample(pad.sample);
      if (auto it = copied.find(src); it != copied.end()) {
        pad.sample = it->second;
        continue;
      }
      if (!fs::is_regular_file(src, ec)) {
        ++missing;
        continue;
      }
      fs::create_directories(dir, ec);
      // Same name, different file: "kick 2.wav". Same file already there: reuse it.
      fs::path dest = dir / src.filename();
      for (int n = 2; fs::exists(dest, ec) && !sameContent(src, dest); ++n)
        dest = dir / (src.stem().string() + " " + std::to_string(n) + src.extension().string());
      if (!fs::exists(dest, ec)) {
        if (!fs::copy_file(src, dest, ec)) {
          error = "can't copy " + src.string() + ": " + ec.message();
          return -1;
        }
        ++count;
      }
      const std::string stored = (fs::path("samples") / dest.filename()).string();
      copied[src] = stored;
      pad.sample = stored;
    }
  }
  if (!copied.empty()) markDirty();
  if (missing) error = std::to_string(missing) + " sample" + (missing == 1 ? " is" : "s are") + " missing and stayed as they were";
  return count;
}

bool Session::saveKit(int t, const std::string& name, std::string& error) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Drum) {
    error = "not a drum track";
    return false;
  }
  if (!validProjectName(name)) {
    error = "names can use letters, digits, spaces, - _ and .";
    return false;
  }
  json pads = json::array();
  const auto table = DrumEngine::padParamTable();
  for (int pad = 0; pad < DrumEngine::kPads; ++pad) {
    json jp;
    jp["pad"] = pad + 1;
    const std::string& stored = tr.pads[static_cast<std::size_t>(pad)].sample;
    // Absolute, so the kit works from any project; "~/" keeps it working under another user name.
    jp["sample"] = stored.empty() ? std::string() : abbreviateHome(resolveSample(stored).string());
    json params;
    for (int p = 0; p < DrumEngine::kPadParamCount; ++p)
      params[table[static_cast<std::size_t>(p)].key] = param(engineRef(t, DrumEngine::param(pad, p)));
    jp["params"] = params;
    pads.push_back(jp);
  }
  const json j = {{"ion_kit", 2}, {"name", name}, {"saved_by", versionLine()}, {"saved_at", nowIso()}, {"pads", pads}};
  if (!writeFileAtomic(paths_.kitFile(name), j.dump(2) + "\n", error)) return false;
  tr.kit = name;
  markDirty();
  return true;
}

bool Session::loadKit(int t, const fs::path& file, std::string& error) {
  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Drum) {
    error = "not a drum track";
    return false;
  }
  editLabel("Track " + std::to_string(t + 1) + " · kit " + file.stem().string());
  std::string text;
  if (!readFile(file, text)) {
    error = "can't read " + file.string();
    return false;
  }
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("ion_kit") || !j.contains("pads") || !j["pads"].is_array()) {
    error = "not an ion kit";
    return false;
  }
  // Kits from before the uniform envelope (0.3, 0.4) are version 1.
  if (j["ion_kit"].is_number() && j["ion_kit"].get<int>() < 2)
    for (json& jp : j["pads"])
      if (jp.contains("params")) upgradePadParams(jp["params"], "");
  try {
    const auto table = DrumEngine::padParamTable();
    std::array<const json*, DrumEngine::kPads> byPad{};
    for (const json& jp : j["pads"]) {
      const int pad = jp.value("pad", 0) - 1;
      if (pad >= 0 && pad < DrumEngine::kPads) byPad[static_cast<std::size_t>(pad)] = &jp;
    }
    // Pads the kit doesn't mention are cleared, so the kit sounds the same wherever it's loaded.
    for (int pad = 0; pad < DrumEngine::kPads; ++pad) {
      const json* jp = byPad[static_cast<std::size_t>(pad)];
      for (int p = 0; p < DrumEngine::kPadParamCount; ++p) {
        const ParamInfo& pi = DrumEngine::paramTable()[static_cast<std::size_t>(DrumEngine::param(pad, p))];
        const char* key = table[static_cast<std::size_t>(p)].key;
        float v = pi.def;
        if (jp && jp->contains("params") && (*jp)["params"].contains(key) && (*jp)["params"][key].is_number())
          v = clampValue(pi, (*jp)["params"][key].get<float>());
        if (v != param(engineRef(t, DrumEngine::param(pad, p)))) setParam(engineRef(t, DrumEngine::param(pad, p)), v);
      }
      setPadSample(t, pad, jp ? jp->value("sample", std::string()) : std::string());
    }
  } catch (const json::exception& e) {
    error = std::string("bad kit file: ") + e.what();
    return false;
  }
  tracks_[static_cast<std::size_t>(t)].kit = j.value("name", file.stem().string());
  markDirty();
  return true;
}

std::vector<std::string> Session::listKits() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(paths_.kitsDir, ec))
    if (e.is_regular_file(ec) && e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

// ---- modulation (D-58) -------------------------------------------------------------------

bool Session::modulatable(ParamRef ref) const {
  if (ref.scope >= kTrackCount || ref.isMod()) return false;
  const ParamInfo* pi = paramInfo(ref);
  return pi && pi->curve != Curve::Toggle && pi->curve != Curve::Choice;
}

void Session::sendModTarget(int t, int route) {
  Command c;
  c.type = Command::Type::SetModTarget;
  c.track = static_cast<uint8_t>(t);
  c.slot = route;
  c.value = tracks_[static_cast<std::size_t>(t)].modTargets[static_cast<std::size_t>(route)];
  send(c);
}

std::optional<ParamRef> Session::modTarget(int t, int route) const {
  const int16_t i = tracks_[static_cast<std::size_t>(t)].modTargets[static_cast<std::size_t>(route)];
  if (i < 0) return std::nullopt;
  return ParamRef{static_cast<uint8_t>(t), static_cast<uint16_t>(i)};
}

void Session::setModTarget(int t, int route, std::optional<ParamRef> target) {
  if (t < 0 || t >= kTrackCount || route < 0 || route >= kModRoutes) return;
  if (target && (target->scope != t || !modulatable(*target))) return;
  const int16_t index = target ? static_cast<int16_t>(target->index) : int16_t{-1};
  int16_t& slot = tracks_[static_cast<std::size_t>(t)].modTargets[static_cast<std::size_t>(route)];
  if (slot == index) return;
  editLabel("Track " + std::to_string(t + 1) + " · mod " + std::to_string(route + 1) + (target ? " → " + paramLabel(*target) : std::string(" cleared")));
  slot = index;
  sendModTarget(t, route);
  markDirty();
}

int Session::addModRoute(ParamRef target) {
  if (!modulatable(target)) return -1;
  const int t = target.scope;
  for (int r = 0; r < kModRoutes; ++r) {
    if (modTarget(t, r)) continue;
    setModTarget(t, r, target);
    if (param(routeRef(t, r, kRouteSource)) == static_cast<float>(kSrcNone)) setParam(routeRef(t, r, kRouteSource), static_cast<float>(kSrcLfo1));
    if (param(routeRef(t, r, kRouteDepth)) == 0.0f) setParam(routeRef(t, r, kRouteDepth), 0.25f);
    return r;
  }
  return -1;
}

void Session::dropModTargets(int t, bool engine, int insertSlot) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  for (int r = 0; r < kModRoutes; ++r) {
    const int16_t i = tr.modTargets[static_cast<std::size_t>(r)];
    if (i < 0) continue;
    const ParamRef ref{static_cast<uint8_t>(t), static_cast<uint16_t>(i)};
    const bool gone = engine ? ref.isEngine() : (ref.isInsert() && ref.slot() == insertSlot);
    if (!gone) continue;
    tr.modTargets[static_cast<std::size_t>(r)] = -1;
    sendModTarget(t, r);
  }
}

// ---- Plaits ------------------------------------------------------------------------------

void Session::setPlaitsPatches(int t, std::vector<syx::Voice> patches) {
  if (t < 0 || t >= kTrackCount) return;
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Plaits) return;
  if (static_cast<int>(patches.size()) > kMaxPlaitsPatches) patches.resize(kMaxPlaitsPatches);
  for (syx::Voice& v : patches) v = syx::normalize(v);
  editLabel("Track " + std::to_string(t + 1) + " · 6-op patch list");
  tr.plaitsPatches = std::move(patches);
  sendPlaitsBank(t);
  markDirty();
}

// The 32 slots Plaits' Harmonics sweeps across, filled evenly from the list,
// so the whole knob covers the list however long it is.
void Session::sendPlaitsBank(int t) {
  const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  Command c;
  c.type = Command::Type::SetPlaitsBank;
  c.track = static_cast<uint8_t>(t);
  if (!tr.plaitsPatches.empty()) {
    c.plaitsBank = new PlaitsBank();
    const std::size_t n = tr.plaitsPatches.size();
    for (std::size_t slot = 0; slot < 32; ++slot) {
      const syx::PackedVoice packed = syx::pack(tr.plaitsPatches[slot * n / 32]);
      std::copy(packed.begin(), packed.end(), c.plaitsBank->data.begin() + static_cast<std::ptrdiff_t>(slot * syx::kPackedVoiceSize));
    }
  }
  send(c);
}

bool Session::savePlaitsPreset(int t, const std::string& name, std::string& error) {
  const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Plaits) {
    error = "not a Plaits track";
    return false;
  }
  if (!validProjectName(name)) {
    error = "names can use letters, digits, spaces, - _ and .";
    return false;
  }
  json params;
  const auto table = PlaitsEngine::paramTable();
  for (std::size_t i = 0; i < table.size(); ++i) params[table[i].key] = param(engineRef(t, static_cast<int>(i)));
  json patches = json::array();
  for (const syx::Voice& v : tr.plaitsPatches) patches.push_back({{"name", syx::name(v)}, {"voice", toHex(v.data(), v.size())}});
  const json j = {{"ion_preset", 1}, {"engine", "plaits"}, {"name", name}, {"saved_by", versionLine()},
                  {"saved_at", nowIso()}, {"params", params}, {"patches", patches}};
  return writeFileAtomic(paths_.presetFile("plaits", name), j.dump(2) + "\n", error);
}

bool Session::loadPlaitsPreset(int t, const std::string& name, std::string& error) {
  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Plaits) {
    error = "not a Plaits track";
    return false;
  }
  std::string text;
  if (!readFile(paths_.presetFile("plaits", name), text)) {
    error = "no preset called \"" + name + "\"";
    return false;
  }
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || j.value("engine", std::string()) != "plaits" || !j.contains("params")) {
    error = "not a Plaits preset";
    return false;
  }
  editLabel("Track " + std::to_string(t + 1) + " · preset " + name);
  // Settings the preset leaves out go to their defaults, so it sounds the same anywhere.
  const auto table = PlaitsEngine::paramTable();
  for (std::size_t i = 0; i < table.size(); ++i) {
    float v = table[i].def;
    if (j["params"].contains(table[i].key) && j["params"][table[i].key].is_number()) v = j["params"][table[i].key].get<float>();
    setIfDifferent(engineRef(t, static_cast<int>(i)), v);
  }
  std::vector<syx::Voice> patches;
  if (j.contains("patches") && j["patches"].is_array())
    for (const json& jp : j["patches"]) {
      syx::Voice v{};
      if (fromHex(jp.value("voice", std::string()), v.data(), v.size())) patches.push_back(v);
    }
  if (patches != tracks_[static_cast<std::size_t>(t)].plaitsPatches) setPlaitsPatches(t, std::move(patches));
  markDirty();
  return true;
}

std::vector<std::string> Session::listPlaitsPresets() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(paths_.presetsDir / "plaits", ec))
    if (e.is_regular_file(ec) && e.path().extension() == ".json") out.push_back(e.path().stem().string());
  std::sort(out.begin(), out.end());
  return out;
}

Session::KitSummary Session::kitSummary(const std::string& name) const {
  KitSummary out;
  std::string text;
  if (!readFile(paths_.kitFile(name), text)) return out;
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("pads") || !j["pads"].is_array()) return out;
  try {
    out.savedAt = j.value("saved_at", std::string());
    for (const json& jp : j["pads"]) {
      const int pad = jp.value("pad", 0) - 1;
      if (pad >= 0 && pad < DrumEngine::kPads) out.samples[static_cast<std::size_t>(pad)] = jp.value("sample", std::string());
    }
    out.ok = true;
  } catch (const json::exception&) {
  }
  return out;
}

// ---- tick ----------------------------------------------------------------------------

void Session::tick() {
  commitEdit();  // edits made without the UI (headless, tests) are steps too
  while (auto n = module_.popNotification()) {
    switch (n->type) {
      case Notification::Type::EngineRetired:
        if (n->engine) {
          std::vector<const Sample*> held;
          n->engine->heldSamples(held);
          for (const Sample* smp : held) unpin(smp);
        }
        delete n->engine;
        break;
      case Notification::Type::SampleReleased:
        unpin(n->sample);
        break;
      case Notification::Type::EffectRetired:
        delete n->effect;
        break;
      case Notification::Type::TakeDone:
        finishTake(n->take);
        break;
      case Notification::Type::ProgramsRetired:
        delete n->programs;
        break;
      case Notification::Type::BankRetired:
        delete n->plaitsBank;
        break;
      case Notification::Type::ProgramChanged:
        // The audio thread has already loaded it; keep the document in step.
        if (n->program < programs_.size() && n->track < kTrackCount) {
          TrackState& tr = tracks_[n->track];
          tr.hasVoice = true;
          tr.voice = programs_[n->program].voice;
          tr.voiceSource = programs_[n->program].source;
          linkLibrary(tr);
          loadedAt_[n->track] = std::chrono::steady_clock::now();
          undoDoc_.tracks[n->track] = tr;  // playing, not an edit: not a step
          markDirty();
          message_ = "program " + std::to_string(n->program + 1) + " → track " + std::to_string(n->track + 1) + ": " +
                     syx::name(tr.voice);
        }
        break;
      case Notification::Type::CcLearned: {
        editLabel("learn cc " + std::to_string(n->cc) + " → " + paramLabel(n->param));
        if (addMapping(n->channel, n->cc, n->param) >= 0)
          message_ = "learned: cc " + std::to_string(n->cc) + " ch " + std::to_string(n->channel + 1) + " → " +
                     paramLabel(n->param);
        else
          message_ = "CC map is full (64 mappings)";
        learning_.reset();
        break;
      }
    }
  }

  handleLoads();

  // Pull in values the audio thread changed on its own (CC moves).
  const ParamStore& store = module_.params();
  for (std::size_t i = 0; i < values_.size(); ++i) {
    const float s = store.get({static_cast<uint8_t>(i / ParamStore::kPerScope), static_cast<uint16_t>(i % ParamStore::kPerScope)});
    if (s != lastStore_[i]) {
      lastStore_[i] = s;
      values_[i] = s;
      undoValues_[i] = s;  // a CC move is playing, not an edit
    }
  }
  commitEdit();  // what a take or a learned CC changed

  // A patch that has stayed on a track for a few seconds counts as used.
  if (library_) {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
      const int64_t id = tracks_[t].libraryId;
      if (id && usedMarked_[t] != id && now - loadedAt_[t] >= std::chrono::seconds(3)) {
        library_->markUsed(id);
        usedMarked_[t] = id;
      }
    }
  }

  if (config_.autosaveMinutes > 0 && !name_.empty() &&
      std::chrono::steady_clock::now() - lastAutosave_ >= std::chrono::minutes(config_.autosaveMinutes)) {
    lastAutosave_ = std::chrono::steady_clock::now();
    if (autosavedDirty_ || values_ != autosaved_) {
      std::string error;
      if (!writeAutosave(error)) message_ = "autosave failed: " + error;
    }
  }
}

std::string Session::takeMessage() { return std::exchange(message_, {}); }

bool Session::dirty() const { return dirty_ || values_ != saved_; }

void Session::autosaveIfNeeded() {
  if (name_.empty() || !dirty() || (!autosavedDirty_ && values_ == autosaved_)) return;
  std::string error;
  writeAutosave(error);
}

// ---- files ----------------------------------------------------------------------------

json Session::recJson() const {
  const RecSetup& r = rec_;
  json tracks = json::array();
  for (int t = 0; t < kTrackCount; ++t)
    if (r.tracks >> t & 1) tracks.push_back(t + 1);
  const char* dest = r.dest == RecSetup::Dest::Sampler ? "sampler" : r.dest == RecSetup::Dest::Drum ? "drum" : "library";
  const char* slice = r.slice == SliceMode::Equal ? "equal" : r.slice == SliceMode::Transients ? "transients" : "chords";
  return {{"source", r.source == RecSource::Master ? "master" : "tracks"}, {"tracks", tracks}, {"tap", r.tap == RecTap::Pre ? "pre" : "post"},
          {"bars", r.bars}, {"start", r.synced ? "next bar" : "now"}, {"send_to", dest}, {"track", r.destTrack + 1},
          {"first_pad", r.firstPad + 1}, {"slice", slice}, {"slices", r.sliceCount}, {"mute_sources", r.muteSources}};
}

void Session::recFromJson(const json& j) {
  RecSetup r;
  r.source = j.value("source", std::string()) == "tracks" ? RecSource::Tracks : RecSource::Master;
  if (j.contains("tracks") && j["tracks"].is_array())
    for (const json& t : j["tracks"])
      if (t.is_number_integer() && t.get<int>() >= 1 && t.get<int>() <= kTrackCount) r.tracks |= static_cast<uint8_t>(1 << (t.get<int>() - 1));
  r.tap = j.value("tap", std::string()) == "pre" ? RecTap::Pre : RecTap::Post;
  const int bars = j.value("bars", 4);
  r.bars = bars == 0 || bars == 1 || bars == 2 || bars == 4 || bars == 8 || bars == 16 ? bars : 4;
  r.synced = j.value("start", std::string("next bar")) != "now";
  const std::string dest = j.value("send_to", std::string());
  r.dest = dest == "sampler" ? RecSetup::Dest::Sampler : dest == "drum" ? RecSetup::Dest::Drum : RecSetup::Dest::Library;
  r.destTrack = std::clamp(j.value("track", 1) - 1, 0, kTrackCount - 1);
  r.firstPad = std::clamp(j.value("first_pad", 1) - 1, 0, DrumEngine::kPads - 1);
  const std::string slice = j.value("slice", std::string());
  r.slice = slice == "equal" ? SliceMode::Equal : slice == "transients" ? SliceMode::Transients : SliceMode::Chords;
  const int n = j.value("slices", 8);
  r.sliceCount = n == 4 || n == 16 ? n : 8;
  r.muteSources = j.value("mute_sources", true);
  rec_ = r;
}

void Session::reset() {
  cancelLearn();
  rec_ = {};
  // Recordings belong to the project being left: one still running is dropped.
  cancelTake();
  takeAbandoned_ = take_ != nullptr;
  review_.reset();
  for (int t = 0; t < kTrackCount; ++t) {
    if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::None) {
      Command c;
      c.type = Command::Type::SetEngine;
      c.track = static_cast<uint8_t>(t);
      send(c);
    }
    tracks_[static_cast<std::size_t>(t)] = {};
    setChannel(t, t);
    for (int r = 0; r < kModRoutes; ++r) sendModTarget(t, r);
  }
  for (int i = 0; i < kMaxCcMappings; ++i)
    if (cc_[static_cast<std::size_t>(i)].used) removeMapping(i);
  if (!programs_.empty()) {
    programs_.clear();
    sendPrograms();
  }
  padLoads_ = {};
  for (int scope = 0; scope < kScopeCount; ++scope)
    for (int slot = 0; slot < kInsertSlots; ++slot)
      if (!insertFixed(scope, slot) && insert(scope, slot) != EffectType::None) setInsert(scope, slot, EffectType::None);
  const Values before = values_;
  defaultValues();
  for (std::size_t i = 0; i < values_.size(); ++i) {
    if (values_[i] == before[i]) continue;
    const ParamRef ref{static_cast<uint8_t>(i / ParamStore::kPerScope), static_cast<uint16_t>(i % ParamStore::kPerScope)};
    if (!ref.isEngine()) setValue(ref, values_[i]);
  }
  // Every project has a glue compressor on the master (HANDOFF §7.4), set up
  // gently and switched out, so a project sounds the same until it's turned on.
  setInsert(kMasterScope, 0, EffectType::Compressor);
  setParam(insertParamRef(kMasterScope, 0, Compressor::kThreshold), -12);
  setParam(insertParamRef(kMasterScope, 0, Compressor::kRatio), 2);
  setParam(insertParamRef(kMasterScope, 0, Compressor::kAttack), 10);
  setParam(insertParamRef(kMasterScope, 0, Compressor::kRelease), 200);
  setParam(insertOnRef(kMasterScope, 0), 0);
}

void Session::newProject(const std::string& name) {
  reset();
  name_ = name;
  saved_ = autosaved_ = values_;
  dirty_ = autosavedDirty_ = false;
  clearHistory();
}

namespace {

// The strip (fader, pan, EQ, sends) of any scope.
json mixerJson(const Session& s, int scope) {
  json mixer;
  for (int i = 0; i < kMixerParamCount; ++i)
    mixer[mixerParams()[static_cast<std::size_t>(i)].key] = s.param(mixerRef(scope, i));
  return mixer;
}

}  // namespace

json Session::insertsJson(int scope) const {
  json out = json::array();
  for (int slot = 0; slot < kInsertSlots; ++slot) {
    const EffectType type = insert(scope, slot);
    if (type == EffectType::None) continue;
    json jp;
    const auto table = effectParams(type);
    for (std::size_t i = 0; i < table.size(); ++i) jp[table[i].key] = param(insertParamRef(scope, slot, static_cast<int>(i)));
    out.push_back({{"slot", slot + 1}, {"type", effectKey(type)}, {"on", param(insertOnRef(scope, slot)) >= 0.5f}, {"params", jp}});
  }
  return out;
}

void Session::insertsFromJson(int scope, const json& arr) {
  if (!arr.is_array()) return;
  for (const json& ji : arr) {
    const int slot = ji.value("slot", 0) - 1;
    if (slot < 0 || slot >= kInsertSlots) continue;
    const EffectType type = effectFromKey(ji.value("type", std::string("none")).c_str());
    if (!insertFixed(scope, slot)) setInsert(scope, slot, type);
    if (insert(scope, slot) != type || type == EffectType::None) continue;
    setIfDifferent(insertOnRef(scope, slot), ji.value("on", true) ? 1.0f : 0.0f);
    if (!ji.contains("params") || !ji["params"].is_object()) continue;
    const auto table = effectParams(type);
    for (std::size_t i = 0; i < table.size(); ++i)
      if (ji["params"].contains(table[i].key) && ji["params"][table[i].key].is_number())
        setIfDifferent(insertParamRef(scope, slot, static_cast<int>(i)), ji["params"][table[i].key].get<float>());
  }
}

void Session::mixerFromJson(int scope, const json& mixer) {
  if (!mixer.is_object()) return;
  for (int i = 0; i < kMixerParamCount; ++i) {
    const ParamInfo& pi = mixerParams()[static_cast<std::size_t>(i)];
    if (mixer.contains(pi.key) && mixer[pi.key].is_number()) setIfDifferent(mixerRef(scope, i), mixer[pi.key].get<float>());
  }
}

// Loading sends only what differs from what's there (the defaults, after
// reset()): a full project must fit through the command queue with nothing
// draining it (ion-render, make-demo).
void Session::setIfDifferent(ParamRef ref, float value) {
  const ParamInfo* pi = paramInfo(ref);
  if (pi && clampValue(*pi, value) != param(ref)) setParam(ref, value);
}

json Session::trackJson(int t) const {
  const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  json jt;
  jt["track"] = t + 1;
  jt["engine"] = engineKey(tr.engine);
  jt["channel"] = tr.channel + 1;
  jt["mixer"] = mixerJson(*this, t);
  if (json fx = insertsJson(t); !fx.empty()) jt["inserts"] = fx;
  if (tr.engine != EngineType::None) {
    json je;
    const auto table = engineParams(tr.engine);
    json params;
    for (std::size_t i = 0; i < table.size(); ++i) params[table[i].key] = param(engineRef(t, static_cast<int>(i)));
    je["params"] = params;
    if (tr.engine == EngineType::Fm && tr.hasVoice) {
      je["name"] = syx::name(tr.voice);
      je["source"] = tr.voiceSource;
      je["voice"] = toHex(tr.voice.data(), tr.voice.size());
    }
    if (tr.engine == EngineType::Drum) {
      json pads = json::array();
      for (int pad = 0; pad < DrumEngine::kPads; ++pad)
        if (!tr.pads[static_cast<std::size_t>(pad)].sample.empty())
          pads.push_back({{"pad", pad + 1}, {"sample", tr.pads[static_cast<std::size_t>(pad)].sample}});
      je["kit"] = tr.kit;
      // "name" is what the Library screen shows for the track.
      je["name"] = !tr.kit.empty() ? tr.kit : std::to_string(pads.size()) + (pads.size() == 1 ? " pad" : " pads");
      je["pads"] = pads;
    }
    if (tr.engine == EngineType::Plaits) {
      je["name"] = PlaitsEngine::modelName(static_cast<int>(std::lround(param(engineRef(t, PlaitsEngine::kModel)))));
      json patches = json::array();
      for (const syx::Voice& v : tr.plaitsPatches) patches.push_back({{"name", syx::name(v)}, {"voice", toHex(v.data(), v.size())}});
      if (!patches.empty()) je["patches"] = patches;
    }
    if (tr.engine == EngineType::Sampler) {
      je["sample"] = tr.pads[0].sample;
      je["name"] = tr.pads[0].sample.empty() ? std::string() : fs::path(tr.pads[0].sample).stem().string();
    }
    jt[engineKey(tr.engine)] = je;
  }
  // Modulation: its settings, and each route's target by its key within the track ("sampler.cutoff", "fader").
  json jm, mp;
  for (int i = 0; i < kModParamCount; ++i)
    mp[modParams()[static_cast<std::size_t>(i)].key] = param({static_cast<uint8_t>(t), static_cast<uint16_t>(kModBase + i)});
  jm["params"] = mp;
  json routes = json::array();
  const std::string prefix = scopeKey(t) + ".";
  for (int r = 0; r < kModRoutes; ++r)
    if (const auto target = modTarget(t, r)) {
      const std::string key = paramKey(*target);
      if (key.rfind(prefix, 0) == 0) routes.push_back({{"route", r + 1}, {"target", key.substr(prefix.size())}});
    }
  jm["routes"] = routes;
  jt["mod"] = jm;
  return jt;
}

std::string Session::toJson() const {
  json j;
  j["format"] = kProjectFormat;
  j["saved_by"] = versionLine();
  j["saved_at"] = nowIso();
  json tracks = json::array();
  for (int t = 0; t < kTrackCount; ++t) tracks.push_back(trackJson(t));
  j["tracks"] = tracks;
  j["master"] = {{"mixer", mixerJson(*this, kMasterScope)}, {"inserts", insertsJson(kMasterScope)}};
  j["returns"] = {{"delay", {{"mixer", mixerJson(*this, kDelayScope)}, {"inserts", insertsJson(kDelayScope)}}},
                  {"reverb", {{"mixer", mixerJson(*this, kReverbScope)}, {"inserts", insertsJson(kReverbScope)}}}};
  json cc = json::array();
  for (const CcEntry& e : ccMappings()) {
    json jc;
    jc["cc"] = e.mapping.cc;
    jc["channel"] = e.mapping.channel == kAnyChannel ? json("any") : json(e.mapping.channel + 1);
    jc["target"] = paramKey(e.mapping.target);
    jc["lo"] = e.mapping.lo;
    jc["hi"] = e.mapping.hi;
    cc.push_back(jc);
  }
  j["cc"] = cc;
  json programs = json::array();
  for (const ProgramEntry& p : programs_)
    programs.push_back({{"name", syx::name(p.voice)}, {"source", p.source}, {"voice", toHex(p.voice.data(), p.voice.size())}});
  j["programs"] = programs;
  j["rec"] = recJson();
  return j.dump(2) + "\n";
}

// One track as saved, onto track t: on a fresh project (load) or over what's
// there (undoing a take). Engine settings start from the engine's defaults.
void Session::trackFromJson(int t, const json& jt) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  // Over a live track: start from an empty one (the channel stays).
  tr.hasVoice = false;
  tr.voiceSource.clear();
  tr.libraryId = 0;
  tr.pads = {};
  tr.kit.clear();
  tr.plaitsPatches.clear();
  for (int r = 0; r < kModRoutes; ++r)
    if (tr.modTargets[static_cast<std::size_t>(r)] >= 0) {
      tr.modTargets[static_cast<std::size_t>(r)] = -1;
      sendModTarget(t, r);
    }
  setChannel(t, jt.value("channel", t + 1) - 1);
  if (jt.contains("mixer")) mixerFromJson(t, jt["mixer"]);
  if (jt.contains("inserts")) insertsFromJson(t, jt["inserts"]);
  const EngineType type = engineFromKey(jt.value("engine", std::string("none")).c_str());
  if (type == EngineType::None) {
    if (tr.engine != EngineType::None) setEngine(t, EngineType::None);
    modFromJson(t, jt);
    return;
  }
  const json je = jt.contains(engineKey(type)) ? jt[engineKey(type)] : json::object();
  if (type == EngineType::Fm && je.contains("voice")) {
    syx::Voice v{};
    if (fromHex(je["voice"].get<std::string>(), v.data(), v.size())) {
      tr.hasVoice = true;
      tr.voice = syx::normalize(v);
      tr.voiceSource = je.value("source", std::string());
      linkLibrary(tr);
    }
  }
  if (type == EngineType::Drum) {
    tr.kit = je.value("kit", std::string());
    if (je.contains("pads") && je["pads"].is_array())
      for (const json& jp : je["pads"]) {
        const int pad = jp.value("pad", 0) - 1;
        if (pad >= 0 && pad < DrumEngine::kPads) tr.pads[static_cast<std::size_t>(pad)].sample = jp.value("sample", std::string());
      }
  }
  if (type == EngineType::Sampler) tr.pads[0].sample = je.value("sample", std::string());
  if (type == EngineType::Plaits && je.contains("patches") && je["patches"].is_array())
    for (const json& jp : je["patches"]) {
      syx::Voice v{};
      if (static_cast<int>(tr.plaitsPatches.size()) < kMaxPlaitsPatches && fromHex(jp.value("voice", std::string()), v.data(), v.size()))
        tr.plaitsPatches.push_back(syx::normalize(v));
    }
  setEngine(t, type);
  if (je.contains("params") && je["params"].is_object()) {
    // The new engine is already at its defaults: send only what differs
    // (a kit has 272 parameters, and the command queue holds 2048).
    const auto table = engineParams(type);
    for (std::size_t i = 0; i < table.size(); ++i)
      if (je["params"].contains(table[i].key) && je["params"][table[i].key].is_number()) {
        const float v = clampValue(table[i], je["params"][table[i].key].get<float>());
        if (v != param(engineRef(t, static_cast<int>(i)))) setParam(engineRef(t, static_cast<int>(i)), v);
      }
  }
  modFromJson(t, jt);
}

// A track's modulation as saved (format 8); older files have none and keep the defaults.
void Session::modFromJson(int t, const json& jt) {
  if (!jt.contains("mod") || !jt["mod"].is_object()) return;
  const json& jm = jt["mod"];
  if (jm.contains("params") && jm["params"].is_object())
    for (int i = 0; i < kModParamCount; ++i) {
      const char* key = modParams()[static_cast<std::size_t>(i)].key;
      if (jm["params"].contains(key) && jm["params"][key].is_number())
        setIfDifferent({static_cast<uint8_t>(t), static_cast<uint16_t>(kModBase + i)}, jm["params"][key].get<float>());
    }
  if (jm.contains("routes") && jm["routes"].is_array())
    for (const json& jr : jm["routes"]) {
      const int r = jr.value("route", 0) - 1;
      if (r < 0 || r >= kModRoutes) continue;
      if (const auto target = parseParamKey(scopeKey(t) + "." + jr.value("target", std::string())))
        setModTarget(t, r, target);
    }
}

bool Session::fromJson(const std::string& text, std::string& error) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    error = "not valid JSON";
    return false;
  }
  if (!migrate(j, error)) return false;

  try {
    reset();
    if (j.contains("tracks") && j["tracks"].is_array()) {
      for (const json& jt : j["tracks"]) {
        const int t = jt.value("track", 0) - 1;
        if (t >= 0 && t < kTrackCount) trackFromJson(t, jt);
      }
    }
    if (j.contains("master") && j["master"].is_object()) {
      const json& m = j["master"];
      if (m.contains("mixer")) mixerFromJson(kMasterScope, m["mixer"]);
      else if (m.contains("fader") && m["fader"].is_number()) setParam(mixerRef(kMasterScope, kFader), m["fader"].get<float>());  // format ≤ 3
      if (m.contains("inserts")) insertsFromJson(kMasterScope, m["inserts"]);
    }
    if (j.contains("returns") && j["returns"].is_object())
      for (int scope : {kDelayScope, kReverbScope}) {
        const std::string key = scopeKey(scope);
        if (!j["returns"].contains(key)) continue;
        const json& jr = j["returns"][key];
        if (jr.contains("mixer")) mixerFromJson(scope, jr["mixer"]);
        if (jr.contains("inserts")) insertsFromJson(scope, jr["inserts"]);
      }
    if (j.contains("cc") && j["cc"].is_array()) {
      int slot = 0;
      for (const json& jc : j["cc"]) {
        if (slot >= kMaxCcMappings) break;
        const auto target = parseParamKey(jc.value("target", std::string()));
        if (!target) continue;
        CcMapping m;
        m.used = true;
        m.cc = static_cast<uint8_t>(std::clamp(jc.value("cc", 0), 0, 127));
        m.channel = jc.contains("channel") && jc["channel"].is_number()
                        ? static_cast<uint8_t>(std::clamp(jc["channel"].get<int>() - 1, 0, 15))
                        : kAnyChannel;
        m.target = *target;
        m.lo = jc.value("lo", 0.0f);
        m.hi = jc.value("hi", 1.0f);
        cc_[static_cast<std::size_t>(slot)] = m;
        Command c;
        c.type = Command::Type::SetCcMapping;
        c.slot = slot;
        c.mapping = m;
        send(c);
        ++slot;
      }
    }
    if (j.contains("programs") && j["programs"].is_array()) {
      for (const json& jp : j["programs"]) {
        if (static_cast<int>(programs_.size()) >= kMaxPrograms) break;
        ProgramEntry e;
        if (!fromHex(jp.value("voice", std::string()), e.voice.data(), e.voice.size())) continue;
        e.voice = syx::normalize(e.voice);
        e.source = jp.value("source", std::string());
        programs_.push_back(e);
      }
      sendPrograms();
    }
    if (j.contains("rec") && j["rec"].is_object()) recFromJson(j["rec"]);
  } catch (const json::exception& e) {
    error = std::string("bad project file: ") + e.what();
    clearHistory();
    return false;
  }
  clearHistory();
  return true;
}

bool Session::load(const std::string& name, std::string& error) {
  std::string text;
  if (!readFile(paths_.projectFile(name), text)) {
    error = "no project called \"" + name + "\"";
    return false;
  }
  // The name first: pad samples collected into the project resolve against its folder.
  const std::string old = std::exchange(name_, name);
  if (!fromJson(text, error)) {
    name_ = old;
    return false;
  }
  saved_ = autosaved_ = values_;
  dirty_ = autosavedDirty_ = false;
  rememberLastProject();
  return true;
}

bool Session::save(std::string& error) {
  if (name_.empty()) {
    error = "the project has no name";
    return false;
  }
  if (!writeFileAtomic(paths_.projectFile(name_), toJson(), error)) return false;
  std::error_code ec;
  fs::remove(paths_.autosaveFile(name_), ec);
  saved_ = autosaved_ = values_;
  dirty_ = autosavedDirty_ = false;
  rememberLastProject();
  return true;
}

bool Session::saveAs(const std::string& name, std::string& error) {
  if (!validProjectName(name)) {
    error = "names can use letters, digits, spaces, - _ and .";
    return false;
  }
  if (name != name_ && fs::exists(paths_.projectFile(name))) {
    error = "\"" + name + "\" already exists";
    return false;
  }
  const std::string old = name_;
  // Collected samples live in the project folder: they go with it.
  std::error_code ec;
  // …and so do its recordings (pads and samplers point into both).
  for (const char* folder : {"samples", "recordings"}) {
    const fs::path from = paths_.projectDir(old) / folder;
    if (old.empty() || name == old || !fs::is_directory(from, ec)) continue;
    fs::create_directories(paths_.projectDir(name), ec);
    fs::copy(from, paths_.projectDir(name) / folder, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
      error = std::string("can't copy the project's ") + folder + ": " + ec.message();
      return false;
    }
  }
  name_ = name;
  if (!save(error)) {
    name_ = old;
    return false;
  }
  return true;
}

bool Session::writeAutosave(std::string& error) {
  if (!writeFileAtomic(paths_.autosaveFile(name_), toJson(), error)) return false;
  autosaved_ = values_;
  autosavedDirty_ = false;
  // So the next start comes back here and offers the recovery, even for a
  // sketch that was never saved.
  rememberLastProject();
  return true;
}

bool Session::hasRecovery(const std::string& name) const {
  std::error_code ec;
  const fs::path autosave = paths_.autosaveFile(name);
  if (!fs::exists(autosave, ec)) return false;
  const fs::path project = paths_.projectFile(name);
  if (!fs::exists(project, ec)) return true;
  return fs::last_write_time(autosave, ec) > fs::last_write_time(project, ec);
}

bool Session::recover(const std::string& name, std::string& error) {
  std::string text;
  if (!readFile(paths_.autosaveFile(name), text)) {
    error = "no autosave for \"" + name + "\"";
    return false;
  }
  const std::string old = std::exchange(name_, name);
  if (!fromJson(text, error)) {
    name_ = old;
    return false;
  }
  // Recovered changes are unsaved: keep the project marked dirty.
  saved_ = Values{};
  autosaved_ = values_;
  dirty_ = true;
  autosavedDirty_ = false;
  rememberLastProject();
  return true;
}

void Session::discardRecovery() {
  std::error_code ec;
  fs::remove(paths_.autosaveFile(name_), ec);
}

std::vector<std::string> Session::listProjects() const {
  std::vector<std::pair<fs::file_time_type, std::string>> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(paths_.projectsDir, ec)) {
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    const fs::path f = paths_.projectFile(name);
    const fs::path a = paths_.autosaveFile(name);
    if (fs::exists(f, ec)) found.emplace_back(fs::last_write_time(f, ec), name);
    else if (fs::exists(a, ec)) found.emplace_back(fs::last_write_time(a, ec), name);
  }
  std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  std::vector<std::string> names;
  for (auto& [time, name] : found) names.push_back(name);
  return names;
}

Session::ProjectSummary Session::projectSummary(const std::string& name) const {
  ProjectSummary out;
  std::string text;
  if (!readFile(paths_.projectFile(name), text) && !readFile(paths_.autosaveFile(name), text)) return out;
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return out;
  try {
    out.savedAt = j.value("saved_at", std::string());
    if (j.contains("tracks") && j["tracks"].is_array())
      for (const json& jt : j["tracks"]) {
        const int t = jt.value("track", 0) - 1;
        if (t < 0 || t >= kTrackCount) continue;
        auto& tr = out.tracks[static_cast<std::size_t>(t)];
        tr.engine = engineFromKey(jt.value("engine", std::string("none")).c_str());
        if (tr.engine != EngineType::None && jt.contains(engineKey(tr.engine)))
          tr.sound = jt[engineKey(tr.engine)].value("name", std::string());
      }
    if (j.contains("programs") && j["programs"].is_array()) out.programs = static_cast<int>(j["programs"].size());
    out.ok = true;
  } catch (const json::exception&) {
  }
  return out;
}

void Session::rememberLastProject() const {
  toml::table t{{"last_project", name_}};
  std::ostringstream out;
  out << "# ion state; safe to delete.\n" << t << "\n";
  std::string error;
  writeFileAtomic(paths_.stateFile(), out.str(), error);
}

std::string Session::lastProject() const {
  toml::parse_result r = toml::parse_file(paths_.stateFile().string());
  if (!r) return {};
  return r.table()["last_project"].value_or(std::string());
}

std::string Session::freshName() const {
  const std::time_t t = std::time(nullptr);
  char date[16];
  std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&t));
  std::string base = std::string("sketch ") + date;
  std::string name = base;
  std::error_code ec;
  for (int n = 2; fs::exists(paths_.projectDir(name), ec); ++n) name = base + " " + std::to_string(n);
  return name;
}

}  // namespace ion
