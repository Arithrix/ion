#include "project/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#include "core/version.h"
#include "engine/fm/fm_engine.h"

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
  return true;
}

}  // namespace

std::unique_ptr<Engine> createEngine(EngineType type) {
  switch (type) {
    case EngineType::Fm: return std::make_unique<FmEngine>();
    default: return nullptr;  // other engines arrive in later phases
  }
}

Session::Session(Module& module, Paths paths, Config& config)
    : module_(module), paths_(std::move(paths)), config_(config) {
  for (int t = 0; t < kTrackCount; ++t) tracks_[static_cast<std::size_t>(t)].channel = t;
  defaultValues();
  lastStore_ = values_;
  saved_ = autosaved_ = values_;
  Command c;
  c.type = Command::Type::SetTiming;
  c.value = config_.timing == MidiTiming::Immediate ? 1.0f : 0.0f;
  send(c);
}

Session::~Session() {
  // Retired engines still waiting in the queue are freed by the Module.
}

void Session::defaultValues() {
  values_.fill(0.0f);
  for (int s = 0; s < ParamStore::kScopes; ++s)
    for (int i = 0; i < kMixerParamCount; ++i)
      values_[static_cast<std::size_t>(ParamStore::index(mixerRef(s, i)))] = mixerParams()[static_cast<std::size_t>(i)].def;
}

void Session::send(const Command& c) {
  // The audio thread drains the queue every 1.3 ms; wait briefly rather than drop.
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (module_.pushCommand(c)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (c.type == Command::Type::SetEngine) delete c.engine;
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
  markDirty();
}

void Session::setVoice(int t, const syx::Voice& voice, const std::string& source) {
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
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
  std::string s = ref.scope == kMasterScope ? "Master" : "Track " + std::to_string(ref.scope + 1);
  s += " · ";
  if (ref.isEngine() && ref.scope < kTrackCount) {
    s += engineLabel(tracks_[ref.scope].engine);
    s += ' ';
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
  if (key.rfind("master.", 0) == 0) {
    scope = kMasterScope;
    rest = key.substr(7);
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
  return {v(0), v(1), v(2), v(3), v(4), v(5)};
}

void Session::resetMacros(int t) {
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

// ---- tick ----------------------------------------------------------------------------

void Session::tick() {
  while (auto n = module_.popNotification()) {
    switch (n->type) {
      case Notification::Type::EngineRetired:
        delete n->engine;
        break;
      case Notification::Type::ProgramsRetired:
        delete n->programs;
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
          markDirty();
          message_ = "program " + std::to_string(n->program + 1) + " → track " + std::to_string(n->track + 1) + ": " +
                     syx::name(tr.voice);
        }
        break;
      case Notification::Type::CcLearned: {
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

  // Pull in values the audio thread changed on its own (CC moves).
  const ParamStore& store = module_.params();
  for (std::size_t i = 0; i < values_.size(); ++i) {
    const float s = store.get({static_cast<uint8_t>(i / ParamStore::kPerScope), static_cast<uint16_t>(i % ParamStore::kPerScope)});
    if (s != lastStore_[i]) {
      lastStore_[i] = s;
      values_[i] = s;
    }
  }

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

void Session::reset() {
  cancelLearn();
  for (int t = 0; t < kTrackCount; ++t) {
    if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::None) {
      Command c;
      c.type = Command::Type::SetEngine;
      c.track = static_cast<uint8_t>(t);
      send(c);
    }
    tracks_[static_cast<std::size_t>(t)] = {};
    setChannel(t, t);
  }
  for (int i = 0; i < kMaxCcMappings; ++i)
    if (cc_[static_cast<std::size_t>(i)].used) removeMapping(i);
  if (!programs_.empty()) {
    programs_.clear();
    sendPrograms();
  }
  const Values before = values_;
  defaultValues();
  for (std::size_t i = 0; i < values_.size(); ++i) {
    if (values_[i] == before[i]) continue;
    const ParamRef ref{static_cast<uint8_t>(i / ParamStore::kPerScope), static_cast<uint16_t>(i % ParamStore::kPerScope)};
    if (!ref.isEngine()) setValue(ref, values_[i]);
  }
}

void Session::newProject(const std::string& name) {
  reset();
  name_ = name;
  saved_ = autosaved_ = values_;
  dirty_ = autosavedDirty_ = false;
}

std::string Session::toJson() const {
  json j;
  j["format"] = kProjectFormat;
  j["saved_by"] = versionLine();
  j["saved_at"] = nowIso();
  json tracks = json::array();
  for (int t = 0; t < kTrackCount; ++t) {
    const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
    json jt;
    jt["track"] = t + 1;
    jt["engine"] = engineKey(tr.engine);
    jt["channel"] = tr.channel + 1;
    json mixer;
    for (int i = 0; i < kMixerParamCount; ++i)
      mixer[mixerParams()[static_cast<std::size_t>(i)].key] = param(mixerRef(t, i));
    jt["mixer"] = mixer;
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
      jt[engineKey(tr.engine)] = je;
    }
    tracks.push_back(jt);
  }
  j["tracks"] = tracks;
  j["master"] = {{"fader", param(mixerRef(kMasterScope, kFader))}};
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
  return j.dump(2) + "\n";
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
        if (t < 0 || t >= kTrackCount) continue;
        TrackState& tr = tracks_[static_cast<std::size_t>(t)];
        setChannel(t, jt.value("channel", t + 1) - 1);
        if (jt.contains("mixer") && jt["mixer"].is_object()) {
          for (int i = 0; i < kMixerParamCount; ++i) {
            const ParamInfo& pi = mixerParams()[static_cast<std::size_t>(i)];
            if (jt["mixer"].contains(pi.key) && jt["mixer"][pi.key].is_number())
              setParam(mixerRef(t, i), jt["mixer"][pi.key].get<float>());
          }
        }
        const EngineType type = engineFromKey(jt.value("engine", std::string("none")).c_str());
        if (type == EngineType::None) continue;
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
        setEngine(t, type);
        if (je.contains("params") && je["params"].is_object()) {
          const auto table = engineParams(type);
          for (std::size_t i = 0; i < table.size(); ++i)
            if (je["params"].contains(table[i].key) && je["params"][table[i].key].is_number())
              setParam(engineRef(t, static_cast<int>(i)), je["params"][table[i].key].get<float>());
        }
      }
    }
    if (j.contains("master") && j["master"].contains("fader"))
      setParam(mixerRef(kMasterScope, kFader), j["master"]["fader"].get<float>());
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
  } catch (const json::exception& e) {
    error = std::string("bad project file: ") + e.what();
    return false;
  }
  return true;
}

bool Session::load(const std::string& name, std::string& error) {
  std::string text;
  if (!readFile(paths_.projectFile(name), text)) {
    error = "no project called \"" + name + "\"";
    return false;
  }
  if (!fromJson(text, error)) return false;
  name_ = name;
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
  if (!fromJson(text, error)) return false;
  name_ = name;
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
