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
//   2 → 3: drum tracks (pads, kit name). Older files have none: nothing to do.
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
    case EngineType::Drum: return std::make_unique<DrumEngine>();
    default: return nullptr;  // other engines arrive in later phases
  }
}

Session::Session(Module& module, Paths paths, Config& config)
    : module_(module), paths_(std::move(paths)), config_(config), loader_(std::make_unique<SampleLoader>()) {
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
  if (c.type == Command::Type::SetPrograms) delete c.programs;
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
  // The new engine has empty pads: load the track's samples onto it (or, for
  // another engine, forget them).
  padLoads_[static_cast<std::size_t>(t)] = {};
  if (type == EngineType::Drum) {
    for (int pad = 0; pad < DrumEngine::kPads; ++pad) requestPad(t, pad);
  } else {
    tr.pads = {};
    tr.kit.clear();
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

// ---- drum kits -------------------------------------------------------------------------

fs::path Session::resolveSample(const std::string& stored) const {
  if (stored.empty()) return {};
  const fs::path p = expandHome(stored);
  return p.is_absolute() || name_.empty() ? p : paths_.projectDir(name_) / p;
}

void Session::setPadSample(int t, int pad, const std::string& path) {
  if (t < 0 || t >= kTrackCount || pad < 0 || pad >= DrumEngine::kPads) return;
  TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  if (tr.engine != EngineType::Drum) return;
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
  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Drum) return;
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
  const fs::path dir = paths_.projectDir(name_) / "samples";
  std::map<fs::path, std::string> copied;  // source → new stored path
  int count = 0, missing = 0;
  std::error_code ec;
  for (TrackState& tr : tracks_) {
    if (tr.engine != EngineType::Drum) continue;
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
  const json j = {{"ion_kit", 1}, {"name", name}, {"saved_by", versionLine()}, {"saved_at", nowIso()}, {"pads", pads}};
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
  std::string text;
  if (!readFile(file, text)) {
    error = "can't read " + file.string();
    return false;
  }
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("ion_kit") || !j.contains("pads") || !j["pads"].is_array()) {
    error = "not an ion kit";
    return false;
  }
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
  while (auto n = module_.popNotification()) {
    switch (n->type) {
      case Notification::Type::EngineRetired:
        if (n->engine && n->engine->type() == EngineType::Drum)
          static_cast<const DrumEngine*>(n->engine)->forEachSample([&](const Sample* smp) { unpin(smp); });
        delete n->engine;
        break;
      case Notification::Type::SampleReleased:
        unpin(n->sample);
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

  handleLoads();

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
  padLoads_ = {};
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
        if (type == EngineType::Drum) {
          tr.kit = je.value("kit", std::string());
          if (je.contains("pads") && je["pads"].is_array())
            for (const json& jp : je["pads"]) {
              const int pad = jp.value("pad", 0) - 1;
              if (pad >= 0 && pad < DrumEngine::kPads) tr.pads[static_cast<std::size_t>(pad)].sample = jp.value("sample", std::string());
            }
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
  const fs::path samples = paths_.projectDir(old) / "samples";
  if (!old.empty() && name != old && fs::is_directory(samples, ec)) {
    fs::create_directories(paths_.projectDir(name), ec);
    fs::copy(samples, paths_.projectDir(name) / "samples", fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) {
      error = "can't copy the project's samples: " + ec.message();
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
